#!/bin/sh
# Early-boot auto-start for RPiPlay on the rooted LG 49UF850V (webOS 2.2).
#
# The boot hook lives at the top of start-devmode.sh, which upstart
# (/etc/init/devmode.conf) runs AS ROOT on every boot while devmode is
# enabled. The towelroot exploit is therefore NOT needed at boot - this
# script is already root; towelroot stays as the fallback for shells that
# arrive unrooted (the 9922 prisoner-ssh authorized_keys wrapper).
#
# What used to break the chain: devmode_enabled is a session-token FILE that
# LG's own script deletes when the session expires (start-devmode.sh then
# curls developer.lge.com and rm -rf s the flag on fail). devmode.conf then
# wipes all of /media/developer - hook, persist scripts, app, all gone.
# Defence, in the spirit of mvpd-autoroot / Homebrew Channel:
#   1. devmode_enabled becomes a DIRECTORY (existence checks still pass)
#   2. the real token is preserved inside it and start-devmode.sh is patched
#      to read it from there, so LG's check keeps succeeding (it currently
#      reports the session valid with hours to spare, and the dev-mode app
#      renews it whenever it is opened)
#   3. everything is self-healed on every boot, so even a wipe repairs itself
#
# Install: copied to /media/developer/persist/startup.sh by install-ipk.sh,
# called from the hook:  LD_PRELOAD="" nohup .../startup.sh & ...
#
# No apostrophes anywhere - the file is pushed through ssh heredocs.

PERSIST=/media/developer/persist
APP_DIR=/media/developer/apps/usr/palm/applications/com.rpiplay.airplay
START_DEVMODE=/media/cryptofs/apps/usr/palm/services/com.palmdts.devmode.service/start-devmode.sh
PREFS=/var/luna/preferences
TOKEN_FILE=$PREFS/devmode_enabled/token
LOGF=/tmp/autoroot.log

log() {
    echo "$(date) startup: $*" >> "$LOGF"
}

# --- once per boot, and never twice at once ----------------------------------
once=/tmp/rpiplay_startup
exec 200>${once}.lock
if ! flock -x -n 200; then
    log "already running, exiting"
    exit 1
fi
trap "rm -f ${once}.lock" EXIT
[ -f "$once" ] && { log "already finished, exiting"; exit 2; }
touch "$once"
log "begin (uid=$(id -u))"

# --- permanent devmode flag ----------------------------------------------------
if [ ! -d "$PREFS/devmode_enabled" ]; then
    # preserve a live token before converting file -> directory
    if [ -f "$PREFS/devmode_enabled" ]; then
        tok=$(cat "$PREFS/devmode_enabled" 2>/dev/null)
        rm -f "$PREFS/devmode_enabled"
        mkdir -p "$PREFS/devmode_enabled"
        [ -n "$tok" ] && printf '%s' "$tok" > "$TOKEN_FILE"
    else
        mkdir -p "$PREFS/devmode_enabled"
    fi
    log "devmode_enabled converted to persistent directory"
fi
[ -f "$TOKEN_FILE" ] || {
    tok=$(ls -t "$PREFS"/devmode_enabled/* 2>/dev/null | head -1)
    [ -n "$tok" ] && cp -f "$tok" "$TOKEN_FILE" 2>/dev/null
}
# If the dev-mode app ever re-created a token file beside an old directory
# (jailer resets), adopt it instead of letting LG see a stale one.
if [ -f "$PREFS/devmode_enabled" ] && [ -f "$TOKEN_FILE" ]; then
    printf '%s' "$(cat "$TOKEN_FILE")" > "$PREFS/devmode_enabled"
fi
rm -f "$PREFS"/dc* 2>/dev/null    # devmode reboot/session counters

# --- patch + self-heal start-devmode.sh ---------------------------------------
if [ -f "$START_DEVMODE" ]; then
    # a) old towelroot-era hook block -> new one; add it when absent
    if ! grep -q "RPIPLAY BOOT HOOK" "$START_DEVMODE" 2>/dev/null; then
        sed -i "/=== AUTOROOT BOOT HOOK ===/,/=== END AUTOROOT ===/d" \
            "$START_DEVMODE" 2>/dev/null
        sed -i '1a\
# === RPIPLAY BOOT HOOK ===\
LD_PRELOAD="" nohup /media/developer/persist/startup.sh > /dev/null 2>\&1 < /dev/null \&\
# === END RPIPLAY HOOK ===' "$START_DEVMODE" 2>/dev/null
        chmod 700 "$START_DEVMODE" 2>/dev/null
        log "boot hook injected into start-devmode.sh"
    fi
    # b) point the session check at the preserved token, so an expired
    #    session can never rm -rf the flag directory (the dev-mode app keeps
    #    the token renewed; worst case this mirrors the old behaviour)
    grep -q "cat /var/luna/preferences/devmode_enabled);" "$START_DEVMODE" 2>/dev/null && \
    ! grep -q "persist/devmode_token" "$START_DEVMODE" 2>/dev/null && {
        mkdir -p "$PERSIST"
        cp -f "$TOKEN_FILE" "$PERSIST/devmode_token" 2>/dev/null
        sed -i "s|cat /var/luna/preferences/devmode_enabled);|cat /media/developer/persist/devmode_token);|" \
            "$START_DEVMODE" 2>/dev/null
        log "session check pointed at persist/devmode_token"
    }
fi

# --- root sshd on port 22 -------------------------------------------------------
# Wait for a real network address (up to ~30s) so sshd binds once networking
# is actually up; skip if something already owns port 22.
i=0
while [ $i -lt 30 ]; do
    netstat -tln 2>/dev/null | grep -q ":22 " && break
    n=$(ifconfig -a 2>/dev/null | grep "inet addr:" | grep -vc "addr:127\.\|addr:169\.254\.")
    [ "${n:-0}" -ge 1 ] && break
    sleep 1
    i=$((i + 1))
done
if ! netstat -tln 2>/dev/null | grep -q ":22 "; then
    mkdir -p /home/root/.ssh
    chmod 700 /home/root/.ssh 2>/dev/null
    chmod 600 "$PERSIST/ssh_host_rsa_key" 2>/dev/null
    if /opt/openssh/sbin/sshd \
        -o StrictModes=no \
        -o PermitRootLogin=yes \
        -o PasswordAuthentication=no \
        -o PermitUserEnvironment=yes \
        -o UseDNS=no \
        -h "$PERSIST/ssh_host_rsa_key" \
        -p 22
    then log "root sshd started on port 22"
    else log "sshd FAILED to start"
    fi
else
    log "port 22 already in use, skipping sshd"
fi

# --- root fallback for unrooted shells (fire-and-forget) -------------------------
# Keeps /tmp/rootsh ready for the prisoner-ssh wrapper; autoroot.sh exits
# straight away when already rooted.
[ -x "$PERSIST/autoroot.sh" ] && "$PERSIST/autoroot.sh" >/dev/null 2>&1 &

# --- RPiPlay supervisor -----------------------------------------------------------
# Waits for the settings service (device name) itself and holds a lock dir, so
# concurrent or early invocations are harmless.
SUP=$APP_DIR/rpiplay-start.sh
[ -x "$SUP" ] || SUP=$PERSIST/rpiplay-start.sh    # pre-1.1 layout fallback
if [ -x "$SUP" ]; then
    LD_PRELOAD="" nohup "$SUP" >/dev/null 2>&1 </dev/null &
    log "rpiplay supervisor launched"
else
    log "no rpiplay-start.sh found - skipping"
fi

log "done"
