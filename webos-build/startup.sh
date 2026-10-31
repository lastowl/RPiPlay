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
LOGF=/tmp/rpiplay-startup.log

log() {
    echo "$(date) startup: $*" >> "$LOGF"
}

# Exact port-22 listener check ("grep :22" also matches :9922).
port22_up() {
    netstat -tln 2>/dev/null | awk '{print $4}' | grep -Eq '[:.]22$'
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
patch_start_devmode() {
    [ -f "$START_DEVMODE" ] || return 0
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
    return 0
}
patch_start_devmode

# --- root sshd on port 22 -------------------------------------------------------
# /opt/openssh only exists inside the dev-mode jail; in the real rootfs the
# sshd binary ships with the devmode service (mmcblk0p47, mounted ro) and
# needs its bundled libs on LD_LIBRARY_PATH.
SSHD=""
for cand in /opt/openssh/sbin/sshd \
    /media/cryptofs/apps/usr/palm/services/com.palmdts.devmode.service/binaries-armv71/opt/openssh/sbin/sshd \
    /media/cryptofs/apps/usr/palm/services/com.palmdts.devmode.service/binaries-i686/opt/openssh/sbin/sshd
do
    [ -x "$cand" ] && { SSHD=$cand; break; }
done

# Wait for a real network address (up to ~30s) so sshd binds once networking
# is actually up; skip if something already owns port 22.
i=0
while [ $i -lt 30 ]; do
    port22_up && break
    n=$(ifconfig -a 2>/dev/null | grep "inet addr:" | grep -vc "addr:127\.\|addr:169\.254\.")
    [ "${n:-0}" -ge 1 ] && break
    sleep 1
    i=$((i + 1))
done
if ! port22_up; then
    if [ -n "$SSHD" ]; then
        mkdir -p /home/root/.ssh /var/run/sshd
        chmod 700 /home/root/.ssh 2>/dev/null
        chmod 600 "$PERSIST/ssh_host_rsa_key" 2>/dev/null
        start_sshd() {
            # /var/run may be tmpfs-mounted over the dir sshd made moments
            # after minimal-boot-done - re-create it on every attempt
            mkdir -p /var/run/sshd
            LD_LIBRARY_PATH="${SSHD%/*}/../lib/openssh${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                "$SSHD" \
                -f /dev/null \
                -o StrictModes=no \
                -o PermitRootLogin=yes \
                -o PasswordAuthentication=no \
                -o PermitUserEnvironment=yes \
                -o UseDNS=no \
                -h "$PERSIST/ssh_host_rsa_key" \
                -p 22 2>> "$LOGF"
        }
        attempt=0
        while [ $attempt -lt 3 ]; do
            attempt=$((attempt + 1))
            if start_sshd; then
                sleep 2
                if port22_up; then
                    log "root sshd listening on port 22 ($SSHD)"
                    break
                fi
                log "sshd exited 0 but is NOT listening (attempt $attempt)"
            else
                log "sshd FAILED to start, rc=$? (attempt $attempt)"
            fi
            sleep 10
        done
    else
        log "no sshd binary found - skipping"
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

# --- late hook re-check ----------------------------------------------------------
# The Dev Mode app re-extracts a pristine start-devmode.sh shortly AFTER it has
# run each boot (appinstalld resync). Wait past that window and re-patch, so
# the on-disk copy is guaranteed hooked for the NEXT boot.
(
    sleep 120
    patch_start_devmode
) &
