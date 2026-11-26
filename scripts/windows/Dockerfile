# Cross-build and test environment for the Windows port (Ubuntu 24.04 host, mingw-w64 GCC 13 with POSIX threads,
# Wine 9 to run the result). Used by scripts/windows/docker-build.sh.
#
# Dependencies built here for x86_64-w64-mingw32 into /opt/win64 (static):
#   OpenSSL   (Apache-2.0)            TLS of the API server, SQLCipher's crypto provider
# SQLCipher is not built here: CMake compiles the vendored amalgamation (third_party/sqlcipher).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates curl rsync perl xz-utils \
    gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev binutils-mingw-w64-x86-64 \
    wine64 libvkd3d-shader1 xvfb xauth x11-utils xdotool libgl1-mesa-dri libglx-mesa0 \
    mesa-vulkan-drivers libgl1 imagemagick fonts-dejavu-core fonts-noto-core winbind cabextract unzip \
 && rm -rf /var/lib/apt/lists/*
RUN update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix \
 && update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

ENV PREFIX=/opt/win64
ENV CROSS=x86_64-w64-mingw32
ARG OPENSSL_VERSION=3.5.4

# OpenSSL: tarball verified against the .sha256 file the project publishes next to it
RUN mkdir /tmp/o && cd /tmp/o \
 && curl -fsSL -O https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz \
 && curl -fsSL -O https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz.sha256 \
 && (cat openssl-${OPENSSL_VERSION}.tar.gz.sha256 | awk '{print $1"  openssl-'${OPENSSL_VERSION}'.tar.gz"}' | sha256sum -c -) \
 && tar xf openssl-${OPENSSL_VERSION}.tar.gz && cd openssl-${OPENSSL_VERSION} \
 && ./Configure mingw64 --cross-compile-prefix=${CROSS}- no-shared no-tests no-apps no-docs --prefix=${PREFIX} --libdir=lib \
 && make -j$(nproc) build_libs && make install_dev \
 && rm -rf /tmp/o

ENV WINEPREFIX=/root/.wine WINEARCH=win64 WINEDEBUG=-all
WORKDIR /work
