#!/bin/sh
# Deploy and (re)start RPiPlay on the TV.
#
# Uses the root sshd on port 22 started by persist/boot-root.sh, then chroots
# into pid 1's root so RPiPlay runs outside the dev-mode jail (luna-send only
# works there). Also installs the com.rpiplay.airplay app files.
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

stop_cmds='
pids=$(pidof rpiplay); [ -n "$pids" ] && kill $pids && sleep 1
luna-send -n 1 -w 3000 luna://com.webos.applicationManager/closeByAppId "{\"id\":\"com.rpiplay.airplay\"}" >/dev/null 2>&1
true
'

if [ "$2" = "stop" ]; then
    echo "$stop_cmds" | tv
    echo "stopped"
    exit 0
fi

echo "=== Uploading binary and app to $TV ==="
ssh -o BatchMode=yes -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -i "$KEY" -p 22 "root@$TV" "cat > $REMOTE_DIR/rpiplay.new" < "$SCRIPT_DIR/output/rpiplay"
COPYFILE_DISABLE=1 tar -C "$SCRIPT_DIR/app" --uid 0 --gid 0 -cf - . | ssh -o BatchMode=yes -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa -i "$KEY" -p 22 "root@$TV" "tar -C $APP_DIR -xf -"

echo "=== Restarting RPiPlay ==="
tv <<EOF
$stop_cmds
mv $REMOTE_DIR/rpiplay.new $REMOTE_DIR/rpiplay
chmod 755 $REMOTE_DIR/rpiplay
nohup $REMOTE_DIR/rpiplay > /tmp/rpiplay.log 2>&1 &
sleep 2
pidof rpiplay >/dev/null && echo "rpiplay running (pid \$(pidof rpiplay))" || { echo "rpiplay failed to start:"; cat /tmp/rpiplay.log; }
EOF
