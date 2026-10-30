FROM ubuntu:24.04
# ^ glibc 2.39: the pinned 2026 SDK host binaries require >= 2.36

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    cmake \
    make \
    pkg-config \
    wget \
    perl \
    tar \
    bzip2 \
    xz-utils \
    python3 \
    libglib2.0-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*

# webOS native SDK (buildroot-nc4 / webosbrew): arm-webos-linux-gnueabi,
# gcc + glibc 2.12, soft-float - one binary covers webOS 1.x+ (it is
# moonlight-tv's toolchain). Pinned release; re-pin deliberately.
# Host-arch-aware: x86_64 SDK in CI, aarch64 SDK on Apple Silicon (native,
# no emulation). The cross-compiler output is identical either way.
ARG SDK_BASE=https://github.com/openlgtv/buildroot-nc4/releases/download/2026.08-webos/arm-webos-linux-gnueabi_sdk-buildroot
RUN case "$(uname -m)" in \
      x86_64)  sdk="${SDK_BASE}-x86_64.tar.gz" ;; \
      aarch64) sdk="${SDK_BASE}-aarch64.tar.gz" ;; \
      *) echo "unsupported build arch: $(uname -m)" >&2; exit 1 ;; \
    esac && \
    wget -q -O /sdk.tar.gz "$sdk" \
    && tar xzf /sdk.tar.gz -C /opt \
    && rm /sdk.tar.gz \
    && /opt/arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
ENV PATH=/opt/arm-webos-linux-gnueabi_sdk-buildroot/bin:$PATH
ENV TCFLAGS="-O2 -march=armv7-a -mfpu=neon-fp16 -mfloat-abi=soft"

WORKDIR /deps

# Cross-compile OpenSSL 1.1.1w against the SDK (static)
RUN wget -q https://www.openssl.org/source/openssl-1.1.1w.tar.gz \
    && tar xf openssl-1.1.1w.tar.gz \
    && cd openssl-1.1.1w \
    && ./Configure linux-armv4 \
       --cross-compile-prefix=arm-webos-linux-gnueabi- \
       --prefix=/deps/openssl-arm \
       no-shared no-async no-engine no-dso \
       $TCFLAGS \
    && make -j$(nproc) \
    && make install_sw \
    && cd .. && rm -rf openssl-1.1.1w*

# Cross-compile libplist for ARM (static)
RUN wget -q https://github.com/libimobiledevice/libplist/releases/download/2.3.0/libplist-2.3.0.tar.bz2 \
    && tar xf libplist-2.3.0.tar.bz2 \
    && cd libplist-2.3.0 \
    && ./configure \
       --host=arm-webos-linux-gnueabi \
       --prefix=/deps/libplist-arm \
       --enable-static --disable-shared \
       --without-cython \
       CC=arm-webos-linux-gnueabi-gcc CXX=arm-webos-linux-gnueabi-g++ \
       CFLAGS="$TCFLAGS" CXXFLAGS="$TCFLAGS" \
    && make -j$(nproc) \
    && make install \
    && cd .. && rm -rf libplist-2.3.0*

# Create isolated GStreamer/glib headers for ARM cross-compilation
# (compile-time only - the final binary links against symbol stubs)
RUN mkdir -p /deps/gst-cross-include && \
    cp -r /usr/include/gstreamer-1.0 /deps/gst-cross-include/ && \
    cp -r /usr/include/glib-2.0 /deps/gst-cross-include/ && \
    mkdir -p /deps/gst-cross-include/glib-config

# ARM32-correct glibconfig.h (plain file + COPY so that every Docker
# parser accepts this Dockerfile - shell-heredoc style needs a very recent
# BuildKit frontend, which CI runners do not always have)
COPY glibconfig-arm32.h /deps/gst-cross-include/glib-config/glibconfig.h

WORKDIR /build
