#!/bin/sh
# Start RPiPlay on the TV and keep it running (restarted if it exits).
#
# Lives in the com.rpiplay.airplay app dir (installed by the ipk from
# package-app.sh) and runs the bundled binary from bin/ there. Must run in
# the TV's real root filesystem, outside the dev-mode jail, because luna-send
# (used to show the player app) only works there. From a jailed root shell:
#   /proc/1/root/bin/busybox chroot /proc/1/root /bin/sh \
#       /media/developer/apps/usr/palm/applications/com.rpiplay.airplay/rpiplay-start.sh
#
# Called at boot from persist/startup.sh and by tv-run.sh and install-ipk.sh.
#
# Usage: rpiplay-start.sh [stop]
# RPIPLAY_ARGS in the environment adds RPiPlay options (e.g. -d for debug logs).

DIR=$(cd "$(dirname "$0")" && pwd)
BIN=$DIR/bin/rpiplay
[ -x "$BIN" ] || BIN=$DIR/rpiplay    # pre-1.1 layout: binary next to the script
LOG=/tmp/rpiplay.log
LOCK=/tmp/rpiplay-start.lock    # held (as a directory) by the running supervisor

# Stops every supervisor and RPiPlay, not just the last one started: copies
# left over from older installs or debug runs all advertise AirPlay and fight
# over the player app.
stop() {
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        [ "$pid" = "$$" ] && continue
        case "$(tr '\0' ' ' < $p/cmdline 2>/dev/null)" in
            *rpiplay-start.sh*) kill "$pid" 2>/dev/null ;;
        esac
    done
    pids=$(pidof rpiplay)
    [ -n "$pids" ] && kill $pids && sleep 1
    rm -rf $LOCK
    luna-send -n 1 -w 3000 luna://com.webos.applicationManager/closeByAppId \
        '{"id":"com.rpiplay.airplay"}' < /dev/null > /dev/null 2>&1
}

if [ "$1" = "stop" ]; then
    stop
    exit 0
fi

# One supervisor at a time; tv-run.sh stops the old one first
if ! mkdir $LOCK 2>/dev/null; then
    echo "rpiplay-start.sh is already running (rpiplay-start.sh stop to restart)"
    exit 0
fi

# Advertise under the TV's own network name (Settings > Network > TV Name).
# At boot the settings service may not be up yet, so keep asking for a while.
i=0
while [ $i -lt 60 ]; do
    name=$(luna-send -n 1 -w 3000 luna://com.webos.settingsservice/getSystemSettings \
        '{"category":"network","keys":["deviceName"]}' < /dev/null 2>/dev/null |
        sed -n 's/.*"deviceName": *"\([^"]*\)".*/\1/p')
    [ -n "$name" ] && break
    sleep 2
    i=$((i + 1))
done
[ -n "$name" ] || name="LG TV"
echo "AirPlay name: $name"

: > $LOG
(
    trap 'exit 0' TERM
    while true; do
        echo "=== rpiplay starting $(date) ===" >> $LOG
        $BIN -n "$name" $RPIPLAY_ARGS >> $LOG 2>&1
        echo "=== rpiplay exited ($?), restarting in 2s ===" >> $LOG
        sleep 2
    done
) < /dev/null > /dev/null 2>&1 &
