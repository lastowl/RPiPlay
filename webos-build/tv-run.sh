#!/bin/sh
# Deploy and (re)start RPiPlay on the TV.
#
# Uses the root sshd on port 22 started by persist/boot-root.sh, then chroots
# into pid 1's root so RPiPlay runs outside the dev-mode jail (luna-send only
# works there). Also installs the com.rpiplay.airplay app files and
# rpiplay-start.sh, which boot-root.sh runs at boot to start RPiPlay.
#
# Usage: ./webos-build/tv-run.sh [tv-ip]        (log: /tmp/rpiplay.log on the TV)
#        ./webos-build/tv-run.sh [tv-ip] stop
set -e

TV="${1:-192.168.50.99}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KEY="$HOME/.ssh/uf850v_webos"
REMOTE_DIR=/media/developer/persist
APP_DIR=/media/developer/apps/usr/palm/applications/com.rpiplay.airplay

tv() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 \
        -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
        -i "$KEY" -p 22 "root@$TV" \
        '/proc/1/root/bin/busybox chroot /proc/1/root /bin/sh -s'
}

START=$REMOTE_DIR/rpiplay-start.sh

if [ "$2" = "stop" ]; then
    echo "$START stop" | tv
    echo "stopped"
    exit 0
fi

echo "=== Uploading binary and app to $TV ==="
ssh -o BatchMode=yes -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -i "$KEY" -p 22 "root@$TV" "cat > $REMOTE_DIR/rpiplay.new" < "$SCRIPT_DIR/output/rpiplay"
ssh -o BatchMode=yes -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -i "$KEY" -p 22 "root@$TV" "cat > $START && chmod 755 $START" < "$SCRIPT_DIR/rpiplay-start.sh"
COPYFILE_DISABLE=1 tar -C "$SCRIPT_DIR/app" --uid 0 --gid 0 -cf - . | ssh -o BatchMode=yes -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa -i "$KEY" -p 22 "root@$TV" "tar -C $APP_DIR -xf -"

echo "=== Restarting RPiPlay ==="
tv <<EOF
$START stop
mv $REMOTE_DIR/rpiplay.new $REMOTE_DIR/rpiplay
chmod 755 $REMOTE_DIR/rpiplay
# Debug: RPIPLAY_DUMP_DIR=/tmp/rpdump ./tv-run.sh saves the H.264 sent after each rotation
${RPIPLAY_DUMP_DIR:+rm -rf $RPIPLAY_DUMP_DIR; mkdir -p $RPIPLAY_DUMP_DIR; export RPIPLAY_DUMP_DIR=$RPIPLAY_DUMP_DIR}
${RPIPLAY_ARGS:+export RPIPLAY_ARGS="$RPIPLAY_ARGS"}
$START
sleep 2
pidof rpiplay >/dev/null && echo "rpiplay running (pid \$(pidof rpiplay))" || { echo "rpiplay failed to start:"; cat /tmp/rpiplay.log; }
EOF
