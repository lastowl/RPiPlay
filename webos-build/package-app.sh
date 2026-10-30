#!/bin/sh
# Build com.rpiplay.airplay_<version>_all.ipk: the web app from webos-build/app
# plus the rpiplay binary (in bin/) and rpiplay-start.sh, so a single file
# contains everything needed to run. Installing it (rather than copying files
# over the app dir) makes the application manager re-read appinfo.json.
#
# Usage: ./webos-build/package-app.sh                      # build only -> output/
#        ./webos-build/package-app.sh install [tv-ip] [ssh-key]
#                                                          # build, then install
#                                                          # on the TV (install-ipk.sh)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_ID=com.rpiplay.airplay
VERSION=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$SCRIPT_DIR/app/appinfo.json")
OUT="$SCRIPT_DIR/output/${APP_ID}_${VERSION}_all.ipk"
BIN="$SCRIPT_DIR/output/rpiplay"
[ -f "$BIN" ] || { echo "no $BIN - run ./webos-build/build.sh first"; exit 1; }
mkdir -p "$SCRIPT_DIR/output"
# Under the project dir (not /var/folders) so Docker Desktop can mount it
WORK=$(mktemp -d "$SCRIPT_DIR/output/.ipk.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

APP="$WORK/data/usr/palm/applications/$APP_ID"
mkdir -p "$APP/bin" "$WORK/data/usr/palm/packages/$APP_ID" "$WORK/control"
cp "$SCRIPT_DIR"/app/* "$APP/"
cp "$BIN" "$APP/bin/rpiplay"
cp "$SCRIPT_DIR/rpiplay-start.sh" "$APP/rpiplay-start.sh"
chmod 755 "$APP/bin/rpiplay" "$APP/rpiplay-start.sh"
cat > "$WORK/data/usr/palm/packages/$APP_ID/packageinfo.json" <<EOF
{
    "id": "$APP_ID",
    "version": "$VERSION",
    "app": "$APP_ID"
}
EOF
cat > "$WORK/control/control" <<EOF
Package: $APP_ID
Version: $VERSION
Section: misc
Priority: optional
Architecture: all
Installed-Size: 1
Description: AirPlay mirroring receiver (RPiPlay) for LG webOS
webOS-Package-Format-Version: 2
webOS-Packager-Version: x
EOF
printf '2.0\n' > "$WORK/debian-binary"

# The TV's opkg can't read macOS bsdtar (pax) archives, so build the
# tarballs with GNU tar (root-owned) in the container
docker run --rm -v "$WORK:/w" rpiplay-webos-builder sh -c '
    cd /w &&
    tar -C control --format=gnu --owner=0 --group=0 -czf control.tar.gz . &&
    tar -C data --format=gnu --owner=0 --group=0 -czf data.tar.gz .'

# Debian-style ar archive: member names without GNU ar's trailing "/",
# which the TV's ar/opkg reject ("unrecognized file type")
python3 -I - "$WORK/pkg.ipk" "$WORK/debian-binary" "$WORK/control.tar.gz" "$WORK/data.tar.gz" <<'PY'
import os, sys, time
with open(sys.argv[1], "wb") as out:
    out.write(b"!<arch>\n")
    for path in sys.argv[2:]:
        data = open(path, "rb").read()
        hdr = "%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (
            os.path.basename(path), int(time.time()), 0, 0, "100644", len(data))
        out.write(hdr.encode())
        out.write(data)
        if len(data) % 2:
            out.write(b"\n")
PY
cp "$WORK/pkg.ipk" "$OUT"
echo "built $OUT"

[ "$1" = "install" ] || exit 0

# Install, chmod, boot hook, start
exec "$SCRIPT_DIR/install-ipk.sh" "$OUT" "${2:-192.168.50.99}" "$3"
