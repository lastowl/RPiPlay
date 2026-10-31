#!/bin/sh
# Install a com.rpiplay.airplay .ipk on a rooted webOS TV and get it running:
#
#   1. installs the package through appinstalld (dev-mode install)
#   2. makes the bundled binary and start script executable (appinstalld may
#      strip the exec bits when extracting)
#   3. points persist/boot-root.sh at the start script so RPiPlay autostarts
#      at boot (creates the file if it isn't there yet)
#   4. removes leftovers of the pre-1.1 layout (binary and script in persist/)
#   5. starts RPiPlay now
#
# Usage: ./webos-build/install-ipk.sh <file.ipk> [tv-ip] [ssh-key]
set -e

IPK=$1
TV=${2:-192.168.50.99}
KEY=${3:-$HOME/.ssh/uf850v_webos}
[ -f "$IPK" ] || { echo "usage: $0 <file.ipk> [tv-ip] [ssh-key]"; exit 1; }

APP_ID=com.rpiplay.airplay
APP_DIR=/media/developer/apps/usr/palm/applications/$APP_ID
BOOT=/media/developer/persist/boot-root.sh
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -i $KEY -p 22 root@$TV"
REMOTE=/media/developer/temp/$APP_ID.ipk

echo "=== Installing $(basename "$IPK") on $TV ==="
$SSH "cat > $REMOTE" < "$IPK"
$SSH '/proc/1/root/bin/busybox chroot /proc/1/root /bin/sh -s' <<EOF
luna-send -n 1 -w 3000 luna://com.webos.applicationManager/closeByAppId '{"id":"$APP_ID"}' < /dev/null > /dev/null 2>&1
# Kill any supervisor regardless of where its copy lives (the new app files
# may not exist yet, so rpiplay-start.sh itself cannot be asked to stop)
for p in /proc/[0-9]*; do
    case "\$(tr '\0' ' ' < \$p/cmdline 2>/dev/null)" in
        *rpiplay-start.sh*) kill "\${p#/proc/}" 2>/dev/null ;;
    esac
done
pids=\$(pidof rpiplay); [ -n "\$pids" ] && kill \$pids 2>/dev/null
# Install, retrying once: appinstalld sometimes stalls in "ipk parsing"
# while it is still busy closing the previous session
i=0
while [ \$i -lt 2 ]; do
    i=\$((i + 1))
    luna-send -n 8 -w 30000 luna://com.webos.appInstallService/dev/install '{"id":"$APP_ID","ipkUrl":"$REMOTE","subscribe":true}' < /dev/null | tail -1
    [ -f $APP_DIR/appinfo.json ] && break
    sleep 3
done
if [ ! -f $APP_DIR/appinfo.json ]; then echo "FAILED: app not deployed to $APP_DIR"; exit 1; fi
rm -f $REMOTE

# The package manager can drop the exec bits from data.tar
chmod 755 $APP_DIR/rpiplay-start.sh $APP_DIR/bin/rpiplay 2>/dev/null
chmod 755 $APP_DIR

# Autostart at boot: keep an rpiplay line in boot-root.sh pointing at the app
# copy (replacing an older one wherever it came from)
[ -f $BOOT ] || printf '#!/bin/sh\n' > $BOOT
grep -v rpiplay-start.sh $BOOT > /tmp/boot-root.new
cat /tmp/boot-root.new > $BOOT
rm -f /tmp/boot-root.new
printf '%s\n' '$APP_DIR/rpiplay-start.sh &' >> $BOOT
chmod 755 $BOOT

# Pre-1.1 layout had the binary and script in persist/
rm -f /media/developer/persist/rpiplay /media/developer/persist/rpiplay-start.sh

$APP_DIR/rpiplay-start.sh stop
$APP_DIR/rpiplay-start.sh
sleep 2
pidof rpiplay >/dev/null && echo "rpiplay running (pid \$(pidof rpiplay)) - AirPlay should show the TV's name" || { echo "rpiplay failed to start:"; cat /tmp/rpiplay.log; }
EOF
