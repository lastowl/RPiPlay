D=/tmp/snapshot-${1:-state}
mkdir -p $D
dmesg > $D/dmesg.txt 2>&1
cat /proc/mounts > $D/mounts.txt
cat /proc/net/udp /proc/net/tcp /proc/net/unix > $D/net.txt 2>&1
ps > $D/ps.txt 2>/dev/null || busybox ps w > $D/ps.txt
ls -lR /proc/1/root/dev > $D/dev-listing.txt 2>&1
cat /proc/1/root/etc/version 2>/dev/null > $D/version.txt
cat /proc/1/root/etc/buildid 2>/dev/null >> $D/version.txt
luna-send -n 1 -w 4000 luna://com.palm.systemservice/getSystemInfo '{"deviceInfo":["firmwareVersion","modelName","cardSerialNumber"]}' < /dev/null > $D/sysinfo.txt 2>&1
ls -l /var/preferences /var/luna/preferences > $D/prefs-listing.txt 2>&1
for P in /proc/[0-9]*; do echo "== ${P#/proc/} $(tr '\0' ' ' < $P/cmdline 2>/dev/null | cut -c1-60)"; ls -l $P/fd 2>/dev/null | awk "{print \$10,\$11}" | grep -a "dev/"; done > $D/fd-map.txt 2>&1
cat /proc/cmdline > $D/cmdline.txt
/proc/1/root/bin/busybox ip link > $D/ip.txt 2>&1
/proc/1/root/bin/busybox ip addr >> $D/ip.txt 2>&1
wifidb=$(find /var/lgnmcs /var/lib/nmcs /var/palm/nmcs -name "*.conf" 2>/dev/null | head -3)
for W in $wifidb; do echo "-- $W"; cat "$W"; done > $D/wifi.txt 2>&1
find /var -name "wpa*supplicant*" -o -name "*psk*" 2>/dev/null | head -5 >> $D/wifi.txt
cp /mnt/lg/cache/webbrowser/chrome.bak/Default/Bookmarks $D/bookmarks.json 2>/dev/null
/proc/1/root/bin/busybox tar czf /tmp/snapshot-${1:-state}.tgz -C /tmp snapshot-${1:-state}
ls -l /tmp/snapshot-${1:-state}.tgz | awk "{print \$5}"
