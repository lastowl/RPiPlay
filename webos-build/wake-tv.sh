#!/bin/sh
# Wake an LG webOS TV from standby with a Wake-on-LAN magic packet.
#
# The TV must have network standby enabled: Settings -> Network ->
# "Mobile TV On" (a.k.a. "Turn on via mobile device") on 2015 models.
# The NIC then stays live in standby and answers to the magic packet on
# UDP 9/7; broadcasts from the LAN are enough (no router config needed).
# Verified on the 49UF850V: reachable again ~15-30 s after the packet.
#
# Usage: ./wake-tv.sh [tv-ip] [mac]        (defaults: this TV)
#        ./wake-tv.sh [tv-ip] [mac] --wait   also wait until ssh answers
set -e

TV="${1:-192.168.50.99}"
MAC="${2:-e8:5b:5b:99:32:14}"
[ "$3" = "--wait" ] && WAIT=1 || WAIT=0

python3 - "$MAC" "$TV" <<'PY'
import socket, binascii, sys, time
mac = binascii.unhexlify(sys.argv[1].replace(":", ""))
tv_ip = sys.argv[2]
subnet = ".".join(tv_ip.split(".")[:3]) + ".255"
data = b"\xff" * 6 + mac * 16
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
for host, port in [(subnet, 9), ("255.255.255.255", 9), (subnet, 7)]:
    try:
        s.sendto(data, (host, port))
    except OSError:
        pass
    time.sleep(0.15)
print("wake packet sent (%s via %s)" % (sys.argv[1], subnet))
PY

if [ "$WAIT" = "1" ]; then
    # the boot chain needs ~30-150 s after the NIC returns; be patient
    for _ in $(seq 1 30); do
        nc -z -G 2 "$TV" 22 2>/dev/null && { echo "tv reachable"; exit 0; }
        sleep 10
    done
    echo "tv still unreachable after 300 s" >&2
    exit 1
fi
