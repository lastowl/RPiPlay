#!/bin/sh
# Keep-awake watcher for the LG 49UF850V (webOS 2.2) - TODO item 4 tier 1.
#
# The TV's autoPowerOff15Min setting powers the panel off mid-session and
# has eaten countless test sessions; while this runs it is forced off every
# minute (idempotent setSettings, no read-back needed on 2.2's service).
# Powering off with the remote still works; this only stops the *automatic*
# sleep. Started by startup.sh at boot; single-instance by /proc check.
#
# Log: /media/developer/persist/powerwatch.log (TICK lines; one per minute).

LOG=/media/developer/persist/powerwatch.log
PIDF=/tmp/powerwatch.pid

# single instance via pidfile (/tmp wipes at power-off, so stale files
# only possible within one boot; kill -0 still catches those)
if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF" 2>/dev/null)" 2>/dev/null; then
    exit 0
fi
echo $$ > "$PIDF"

while true; do
    echo "TICK $(date) up=$(cut -d. -f1 /proc/uptime)" >> "$LOG"
    luna-send -n 1 -w 3000 luna://com.webos.settingsservice/setSystemSettings \
        '{"settings":{"autoPowerOff15Min":false}}' \
        < /dev/null >> "$LOG" 2>&1
    # keep the log tiny (rotate at ~100 KB)
    [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 102400 ] && \
        { tail -100 "$LOG" > "$LOG.t"; mv "$LOG.t" "$LOG"; }
    sleep 60
done
