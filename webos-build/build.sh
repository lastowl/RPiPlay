#!/bin/bash
set -e

# Cross-compile RPiPlay for LG webOS 2.x (ARM soft-float, glibc 2.19)
# Run from the RPiPlay root directory: ./webos-build/build.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_IMAGE="rpiplay-webos-builder"
OUTPUT_DIR="$SCRIPT_DIR/output"

echo "=== Building RPiPlay for webOS ==="
echo "Root: $ROOT_DIR"

# Rebuild Docker image
echo "=== Building Docker cross-compilation environment ==="
docker build -t "$BUILD_IMAGE" "$SCRIPT_DIR"

mkdir -p "$OUTPUT_DIR"

# Run the build inside Docker
# Mount tv-sysroot so we can link against the TV's actual glibc 2.19
docker run --rm \
    -v "$ROOT_DIR:/src:ro" \
    -v "$SCRIPT_DIR:/webos:ro" \
    -v "$OUTPUT_DIR:/output" \
    "$BUILD_IMAGE" \
    bash -c '
set -e

CC=arm-webos-linux-gnueabi-gcc
CXX=arm-webos-linux-gnueabi-g++
# webosbrew SDK (buildroot-nc4 2026.08-webos): glibc 2.12, soft-float - the
# toolchain is built soft-float-only and forces it (verified: no
# Tag_ABI_VFP_args in the output ELF), same ABI as the previous builds.
# A future softfp-only SDK would then fail loudly at link, not silently.
CFLAGS="-O2 -march=armv7-a -mfpu=neon-fp16 -mfloat-abi=soft -fno-stack-protector"
CXXFLAGS="$CFLAGS"

OPENSSL_DIR=/deps/openssl-arm
PLIST_DIR=/deps/libplist-arm

# Isolated GStreamer/glib headers (ARM32 glibconfig.h)
GST_CFLAGS="-I/deps/gst-cross-include/gstreamer-1.0 -I/deps/gst-cross-include/glib-2.0 -I/deps/gst-cross-include/glib-config"

echo "=== Compiling RPiPlay ==="

WORKDIR=/tmp/rpiplay-build
mkdir -p $WORKDIR/obj

# Include paths
INC="-I/src/lib -I/src/lib/playfair -I/src/lib/llhttp -I/src/renderers"
INC="$INC -I$OPENSSL_DIR/include -I$PLIST_DIR/include"
INC="$INC $GST_CFLAGS"

# Defines — no _FILE_OFFSET_BITS=64 / _LARGEFILE64_SOURCE: they redirect to
# 64-bit symbols that the SDK glibc 2.12 does not have
DEFS="-DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS"
DEFS="$DEFS -DTARGET_POSIX -D_LINUX -D_REENTRANT"
DEFS="$DEFS -DHAS_GSTREAMER_RENDERER -DHAS_DUMMY_RENDERER"
DEFS="$DEFS -DOPENSSL_API_COMPAT=0x10101000L"

# Compile playfair
echo "  playfair..."
for f in /src/lib/playfair/*.c; do
    $CC $CFLAGS $DEFS $INC -c "$f" -o "$WORKDIR/obj/$(basename ${f%.c}).o"
done

# Compile llhttp
echo "  llhttp..."
for f in /src/lib/llhttp/*.c; do
    $CC $CFLAGS $DEFS $INC -c "$f" -o "$WORKDIR/obj/$(basename ${f%.c}).o"
done

# Compile airplay lib (skip dnssd.c — replaced by our mDNS)
echo "  airplay lib..."
for f in /src/lib/*.c; do
    if [ "$(basename $f)" = "dnssd.c" ]; then
        echo "    skipping dnssd.c (replaced by webos mDNS)"
        continue
    fi
    $CC $CFLAGS $DEFS $INC -c "$f" -o "$WORKDIR/obj/$(basename ${f%.c}).o"
done

# Compile custom mDNS
echo "  webos mDNS..."
$CC $CFLAGS $DEFS $INC -c /webos/dnssd_mdns.c -o $WORKDIR/obj/dnssd_mdns.o

# Compile renderers
echo "  renderers..."
$CC $CFLAGS $DEFS $INC -c /src/renderers/video_renderer_dummy.c -o $WORKDIR/obj/video_renderer_dummy.o
$CC $CFLAGS $DEFS $INC -c /src/renderers/audio_renderer_dummy.c -o $WORKDIR/obj/audio_renderer_dummy.o

# fdk-aac (AAC-ELD decode + AAC-LC encode for the audio renderer).
# Sources never change, so objects are cached in the output dir.
FDK=/src/renderers/fdk-aac
FDK_INC=""
for d in $FDK/lib*/include; do FDK_INC="$FDK_INC -I$d"; done
FDK_CACHE=/output/.fdk-obj-softfp   # ABI-tagged: old float-abi=soft objects must not be reused
if [ ! -f $FDK_CACHE/.done ]; then
    echo "  fdk-aac (first build, cached afterwards)..."
    mkdir -p $FDK_CACHE
    n=0
    for f in $FDK/lib*/src/*.cpp; do
        o=$FDK_CACHE/fdk_$(basename $(dirname $(dirname $f)))_$(basename ${f%.cpp}).o
        $CXX $CXXFLAGS -fno-exceptions -fno-rtti $FDK_INC -c "$f" -o "$o" &
        n=$((n+1))
        [ $((n % 8)) -eq 0 ] && wait
    done
    wait
    touch $FDK_CACHE/.done
fi
cp $FDK_CACHE/*.o $WORKDIR/obj/

echo "  webos renderers..."
$CC $CFLAGS $DEFS $INC -I/webos -c /webos/video_renderer_webos.c -o $WORKDIR/obj/video_renderer_webos.o
$CC $CFLAGS $DEFS $INC -I/webos $FDK_INC -c /webos/audio_renderer_webos.c -o $WORKDIR/obj/audio_renderer_webos.o

# Compile glibc compat shims (getentropy, explicit_bzero for glibc <2.25)
echo "  glibc compat shims..."
$CC $CFLAGS -c /webos/glibc_compat.c -o $WORKDIR/obj/glibc_compat.o

# Compile main
echo "  main..."
$CXX $CXXFLAGS $DEFS $INC -c /src/rpiplay.cpp -o $WORKDIR/obj/rpiplay.o

# Create stub ARM shared libraries that export GStreamer/glib symbols.
# These provide link-time resolution so the linker creates proper PLT entries.
# At runtime on the TV, the real libraries provide the actual implementations.
echo "=== Creating stub libraries ==="
mkdir -p /deps/stub-libs

# Collect all undefined GStreamer/glib symbols from our object files
arm-webos-linux-gnueabi-nm -u $WORKDIR/obj/*.o 2>/dev/null \
    | grep "U " | sed "s/.*U //" | sort -u \
    | grep -E "^(gst_|g_type_|g_object_|g_error_|_gst_|GST_)" > /tmp/gst_syms.txt

echo "  Found $(wc -l < /tmp/gst_syms.txt) GStreamer/glib symbols to stub"

# Generate C source with stub implementations for all symbols
# _gst_buffer_type is a variable (GType), not a function — handle separately
{
    while IFS= read -r sym; do
        if [ "$sym" = "_gst_buffer_type" ]; then
            echo "unsigned long _gst_buffer_type = 0;"
        else
            echo "void ${sym}(void) {}"
        fi
    done < /tmp/gst_syms.txt
} > /tmp/gst_stubs.c

for lib in gstreamer-1.0 gstapp-1.0 gstvideo-1.0 gobject-2.0 glib-2.0; do
    $CC $CFLAGS -shared /tmp/gst_stubs.c -o /deps/stub-libs/lib${lib}.so \
        -Wl,-soname,lib${lib}.so.0 -nostdlib
    echo "  created stub lib${lib}.so (with real symbol exports)"
done

echo "=== Linking (SDK: glibc 2.12, static libstdc++/libgcc) ==="

# Link order matters: objects, then static libs (OpenSSL, libplist), the
# GStreamer stubs (--no-as-needed keeps NEEDED entries so the TV resolves
# them at runtime), then the maths/pthread/dl system libs.
$CXX $CFLAGS -o /output/rpiplay \
    $WORKDIR/obj/*.o \
    -L$OPENSSL_DIR/lib -lcrypto \
    -L$PLIST_DIR/lib -lplist-2.0 \
    -L/deps/stub-libs -Wl,--no-as-needed \
    -lgstreamer-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgobject-2.0 -lglib-2.0 \
    -Wl,--as-needed \
    -static-libstdc++ -static-libgcc \
    -lm -lpthread -ldl

echo ""
echo "=== Build complete ==="
# `file` is not in the ubuntu image; readelf is (and says more)
arm-webos-linux-gnueabi-readelf -h /output/rpiplay | grep -E "Magic|Machine|Type"
arm-webos-linux-gnueabi-size /output/rpiplay
echo ""
echo "=== Dynamic dependencies ==="
arm-webos-linux-gnueabi-readelf -d /output/rpiplay | grep NEEDED
echo ""
echo "=== Glibc version requirements ==="
arm-webos-linux-gnueabi-readelf -V /output/rpiplay 2>/dev/null | grep -A1 "Version:" || echo "(none found)"
arm-webos-linux-gnueabi-objdump -T /output/rpiplay 2>/dev/null | grep GLIBC | sort -t@ -k2 -V | tail -20 || true
'


echo ""
echo "=== Output ==="
ls -la "$OUTPUT_DIR/rpiplay"
echo "Binary ready at: $OUTPUT_DIR/rpiplay"
