#!/bin/sh
# Deploy and (re)start RPiPlay on the TV.
#
# Uses the root sshd on port 22 started by persist/boot-root.sh, then chroots
# into pid 1's root so RPiPlay runs outside the dev-mode jail (luna-send only
# works there). Deploys the binary, start script and app files into the
# com.rpiplay.airplay app dir — the same layout the ipk from package-app.sh
# installs — and keeps persist/boot-root.sh pointing at the app copy so
# RPiPlay starts again at boot.
#
# Usage: ./webos-build/tv-run.sh [tv-ip]        (log: /tmp/rpiplay.log on the TV)
#        ./webos-build/tv-run.sh [tv-ip] stop
set -e

TV="${1:-192.168.50.99}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KEY="$HOME/.ssh/uf850v_webos"
APP_DIR=/media/developer/apps/usr/palm/applications/com.rpiplay.airplay
BOOT=/media/developer/persist/boot-root.sh
ssh() {
    command ssh -o BatchMode=yes -o ConnectTimeout=8 \
        -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
        -i "$KEY" -p 22 "root@$TV" "$@"
}

tv() {
    ssh '/proc/1/root/bin/busybox chroot /proc/1/root /bin/sh -s'
}

START=$APP_DIR/rpiplay-start.sh

if [ "$2" = "stop" ]; then
    # Either copy kills every supervisor and rpiplay; the persist one is
    # tried too so stop works before the first run of this version
    echo "sh /media/developer/persist/rpiplay-start.sh stop 2>/dev/null; $START stop" | tv
    echo "stopped"
    exit 0
fi

echo "=== Uploading binary and app to $TV ==="
ssh "mkdir -p $APP_DIR/bin && cat > $APP_DIR/bin/rpiplay.new && chmod 755 $APP_DIR/bin/rpiplay.new" < "$SCRIPT_DIR/output/rpiplay"
ssh "cat > $START && chmod 755 $START" < "$SCRIPT_DIR/rpiplay-start.sh"
COPYFILE_DISABLE=1 tar -C "$SCRIPT_DIR/app" --uid 0 --gid 0 -cf - . | ssh "tar -C $APP_DIR -xf -"

echo "=== Restarting RPiPlay ==="
tv <<EOF
$START stop
mv $APP_DIR/bin/rpiplay.new $APP_DIR/bin/rpiplay
# Move off the pre-1.1 layout (binary and script lived in persist/) and keep
# an rpiplay line in boot-root.sh pointing at the app copy
rm -f /media/developer/persist/rpiplay /media/developer/persist/rpiplay-start.sh
[ -f $BOOT ] || printf '#!/bin/sh\n' > $BOOT
grep -v rpiplay-start.sh $BOOT > /tmp/boot-root.new
cat /tmp/boot-root.new > $BOOT
rm -f /tmp/boot-root.new
printf '%s\n' '$START &' >> $BOOT
chmod 755 $BOOT
# Debug: RPIPLAY_DUMP_DIR=/tmp/rpdump ./tv-run.sh saves the H.264 sent after each rotation
${RPIPLAY_DUMP_DIR:+rm -rf $RPIPLAY_DUMP_DIR; mkdir -p $RPIPLAY_DUMP_DIR; export RPIPLAY_DUMP_DIR=$RPIPLAY_DUMP_DIR}
# Debug: RPIPLAY_NO_AUDIO=1 ./tv-run.sh streams video only (latency comparison)
${RPIPLAY_NO_AUDIO:+export RPIPLAY_NO_AUDIO=1}
# Debug: RPIPLAY_LATENCY_TEST=1 ./tv-run.sh serves a clock page at http://<tv>:7737/clock
# for the phone to mirror and overlays the same clock on the TV (see video_renderer_webos.c)
${RPIPLAY_LATENCY_TEST:+export RPIPLAY_LATENCY_TEST=$APP_DIR/clock.html}
${RPIPLAY_ARGS:+export RPIPLAY_ARGS="$RPIPLAY_ARGS"}
$START
sleep 2
pidof rpiplay >/dev/null && echo "rpiplay running (pid \$(pidof rpiplay))" || { echo "rpiplay failed to start:"; cat /tmp/rpiplay.log; }
EOF
