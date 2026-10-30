FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc-arm-linux-gnueabi \
    g++-arm-linux-gnueabi \
    cmake \
    make \
    pkg-config \
    wget \
    tar \
    xz-utils \
    python3 \
    libglib2.0-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /deps

# Cross-compile OpenSSL 1.1.1w for ARM soft-float
RUN wget -q https://www.openssl.org/source/openssl-1.1.1w.tar.gz \
    && tar xf openssl-1.1.1w.tar.gz \
    && cd openssl-1.1.1w \
    && ./Configure linux-armv4 \
       --cross-compile-prefix=arm-linux-gnueabi- \
       --prefix=/deps/openssl-arm \
       no-shared no-async no-engine no-dso \
    && make -j$(nproc) \
    && make install_sw \
    && cd .. && rm -rf openssl-1.1.1w*

# Cross-compile libplist for ARM soft-float
RUN wget -q https://github.com/libimobiledevice/libplist/releases/download/2.3.0/libplist-2.3.0.tar.bz2 \
    && tar xf libplist-2.3.0.tar.bz2 \
    && cd libplist-2.3.0 \
    && ./configure \
       --host=arm-linux-gnueabi \
       --prefix=/deps/libplist-arm \
       --enable-static --disable-shared \
       --without-cython \
    && make -j$(nproc) \
    && make install \
    && cd .. && rm -rf libplist-2.3.0*

# Create isolated GStreamer/glib headers for ARM cross-compilation
RUN mkdir -p /deps/gst-cross-include && \
    cp -r /usr/include/gstreamer-1.0 /deps/gst-cross-include/ && \
    cp -r /usr/include/glib-2.0 /deps/gst-cross-include/ && \
    mkdir -p /deps/gst-cross-include/glib-config

# ARM32-correct glibconfig.h (plain file + COPY so that every Docker
# parser accepts this Dockerfile - shell-heredoc style needs a very recent
# BuildKit frontend, which CI runners do not always have)
COPY glibconfig-arm32.h /deps/gst-cross-include/glib-config/glibconfig.h

WORKDIR /build
