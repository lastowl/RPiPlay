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
    -v "$SCRIPT_DIR/tv-sysroot:/tv-sysroot:ro" \
    -v "$OUTPUT_DIR:/output" \
    "$BUILD_IMAGE" \
    bash -c '
set -e

CC=arm-linux-gnueabi-gcc
CXX=arm-linux-gnueabi-g++
CFLAGS="-O2 -march=armv7-a -mfloat-abi=soft -fno-stack-protector"
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

# Defines — NO _FILE_OFFSET_BITS=64 or _LARGEFILE64_SOURCE
# Those trigger newer glibc symbol redirections incompatible with glibc 2.19
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
FDK_CACHE=/output/.fdk-obj
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

# Assemble custom CRT (targets glibc 2.4+ __libc_start_main ABI)
echo "  custom crt1..."
$CC -c /webos/crt1.S -o $WORKDIR/crt1.o

# Create stub ARM shared libraries that export GStreamer/glib symbols.
# These provide link-time resolution so the linker creates proper PLT entries.
# At runtime on the TV, the real libraries provide the actual implementations.
echo "=== Creating stub libraries ==="
mkdir -p /deps/stub-libs

# Collect all undefined GStreamer/glib symbols from our object files
arm-linux-gnueabi-nm -u $WORKDIR/obj/*.o 2>/dev/null \
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

# Set up linker-friendly symlinks in a writable copy of tv-sysroot
echo "=== Preparing TV sysroot for linking ==="
mkdir -p /tmp/tv-sysroot/lib
cp -a /tv-sysroot/lib/* /tmp/tv-sysroot/lib/
# Ensure linker can find libs via -l flags
cd /tmp/tv-sysroot/lib
for f in libc libm libpthread libdl; do
    if [ ! -e ${f}.so ]; then
        target=$(ls ${f}-*.so ${f}.so.* 2>/dev/null | head -1)
        [ -n "$target" ] && ln -sf "$(basename $target)" ${f}.so
    fi
done
ls -la /tmp/tv-sysroot/lib/
cd /

# Find cross-compiler CRT/GCC paths
CRTI=$($CC -print-file-name=crti.o)
CRTN=$($CC -print-file-name=crtn.o)
CRTBEGIN=$($CC -print-file-name=crtbeginS.o)
CRTEND=$($CC -print-file-name=crtendS.o)
GCC_LIB=$($CC -print-file-name=libgcc.a)
GCC_LIB_DIR=$(dirname $GCC_LIB)
STDCXX_DIR=$(dirname $($CXX -print-file-name=libstdc++.a))

echo "  crti: $CRTI"
echo "  crtn: $CRTN"
echo "  crtbegin: $CRTBEGIN"
echo "  crtend: $CRTEND"
echo "  gcc lib dir: $GCC_LIB_DIR"
echo "  libstdc++ dir: $STDCXX_DIR"

echo "=== Linking with TV glibc 2.19 ==="

# Link order:
# 1. CRT prologue (our custom crt1 + cross-compiler crti/crtbegin)
# 2. All application object files (including glibc_compat.o)
# 3. Static libs (OpenSSL, libplist)
# 4. GStreamer stubs (dynamic, for DT_NEEDED entries)
# 5. Static C++ runtime (libstdc++, libgcc, libgcc_eh)
# 6. TV system libs (libc, libm, libpthread, libdl from glibc 2.19)
# 7. Dynamic gcc support (libgcc_s for unwinding)
# 8. CRT epilogue (crtend, crtn)

arm-linux-gnueabi-ld \
    --dynamic-linker /lib/ld-linux.so.3 \
    -rpath-link /tmp/tv-sysroot/lib \
    $WORKDIR/crt1.o $CRTI $CRTBEGIN \
    $WORKDIR/obj/*.o \
    -L$OPENSSL_DIR/lib -lcrypto \
    -L$PLIST_DIR/lib -lplist-2.0 \
    -L/deps/stub-libs --no-as-needed \
    -lgstreamer-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgobject-2.0 -lglib-2.0 \
    --as-needed \
    -L$STDCXX_DIR -L$GCC_LIB_DIR \
    -Bstatic -lstdc++ -lgcc -lgcc_eh -Bdynamic \
    -L/tmp/tv-sysroot/lib \
    -lm -lpthread -ldl -lc \
    /tmp/tv-sysroot/lib/ld-linux.so.3 \
    -lgcc_s \
    $CRTEND $CRTN \
    -o /output/rpiplay

echo ""
echo "=== Build complete ==="
# `file` is not in the ubuntu image; readelf is (and says more)
arm-linux-gnueabi-readelf -h /output/rpiplay | grep -E "Magic|Machine|Type"
arm-linux-gnueabi-size /output/rpiplay
echo ""
echo "=== Dynamic dependencies ==="
arm-linux-gnueabi-readelf -d /output/rpiplay | grep NEEDED
echo ""
echo "=== Glibc version requirements ==="
arm-linux-gnueabi-readelf -V /output/rpiplay 2>/dev/null | grep -A1 "Version:" || echo "(none found)"
arm-linux-gnueabi-objdump -T /output/rpiplay 2>/dev/null | grep GLIBC | sort -t@ -k2 -V | tail -20 || true
'

echo ""
echo "=== Output ==="
ls -la "$OUTPUT_DIR/rpiplay"
echo "Binary ready at: $OUTPUT_DIR/rpiplay"
