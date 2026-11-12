# Cross-build and test environment for the Windows port (Ubuntu 24.04 host, mingw-w64 GCC 13 with POSIX threads,
# Wine 9 to run the result). Used by scripts/windows/docker-build.sh.
#
# Dependencies built here for x86_64-w64-mingw32 into /opt/win64 (static):
#   zlib      (zlib licence)          SQLCipher and OpenSSL link it
#   OpenSSL   (Apache-2.0)            TLS of the API server, SQLCipher's crypto provider
#   SQLCipher (BSD-3-Clause)          encrypted source libraries; built from its amalgamation
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates curl rsync perl tcl xz-utils \
    gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev binutils-mingw-w64-x86-64 \
    wine64 libvkd3d-shader1 xvfb xauth x11-utils xdotool libgl1-mesa-dri libglx-mesa0 \
    mesa-vulkan-drivers libgl1 imagemagick fonts-dejavu-core fonts-noto-core winbind cabextract unzip \
 && rm -rf /var/lib/apt/lists/*
RUN update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix \
 && update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

ENV PREFIX=/opt/win64
ENV CROSS=x86_64-w64-mingw32
ARG ZLIB_VERSION=1.3.1
ARG OPENSSL_VERSION=3.5.4
ARG SQLCIPHER_VERSION=4.6.1

# zlib (the archive's sha256 is published on zlib.net; pinned here)
RUN mkdir /tmp/z && cd /tmp/z \
 && curl -fsSL -o z.tgz https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz \
 && echo "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23  z.tgz" | sha256sum -c - \
 && tar xf z.tgz && cd zlib-${ZLIB_VERSION} \
 && make -f win32/Makefile.gcc PREFIX=${CROSS}- -j$(nproc) libz.a \
 && install -d ${PREFIX}/lib ${PREFIX}/include && install -m644 libz.a ${PREFIX}/lib && install -m644 zlib.h zconf.h ${PREFIX}/include \
 && rm -rf /tmp/z

# OpenSSL: tarball verified against the .sha256 file the project publishes next to it
RUN mkdir /tmp/o && cd /tmp/o \
 && curl -fsSL -O https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz \
 && curl -fsSL -O https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz.sha256 \
 && (cat openssl-${OPENSSL_VERSION}.tar.gz.sha256 | awk '{print $1"  openssl-'${OPENSSL_VERSION}'.tar.gz"}' | sha256sum -c -) \
 && tar xf openssl-${OPENSSL_VERSION}.tar.gz && cd openssl-${OPENSSL_VERSION} \
 && ./Configure mingw64 --cross-compile-prefix=${CROSS}- no-shared no-tests no-apps no-docs --prefix=${PREFIX} --libdir=lib \
 && make -j$(nproc) build_libs && make install_dev \
 && rm -rf /tmp/o

# SQLCipher: native configure produces the amalgamation (needs tclsh); that one file is cross-compiled.
RUN apt-get update && apt-get install -y --no-install-recommends libssl-dev && rm -rf /var/lib/apt/lists/* \
 && mkdir /tmp/s && cd /tmp/s \
 && curl -fsSL -o sc.tgz https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v${SQLCIPHER_VERSION}.tar.gz \
 && sha256sum sc.tgz | tee /opt/sqlcipher.sha256 \
 && tar xf sc.tgz && cd sqlcipher-${SQLCIPHER_VERSION} \
 && ./configure --disable-tcl --with-tempstore=yes CFLAGS="-DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2" LDFLAGS="-lcrypto" >/tmp/s/configure.log 2>&1 \
 && make sqlite3.c >/dev/null \
 && ${CROSS}-gcc -O2 -w -c sqlite3.c -o sqlite3.o \
      -DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 \
      -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_ENABLE_COLUMN_METADATA -I${PREFIX}/include \
 && ${CROSS}-ar rcs ${PREFIX}/lib/libsqlcipher.a sqlite3.o \
 && install -d ${PREFIX}/include/sqlcipher && install -m644 sqlite3.h ${PREFIX}/include/sqlcipher/ \
 && rm -rf /tmp/s

ENV WINEPREFIX=/root/.wine WINEARCH=win64 WINEDEBUG=-all
WORKDIR /work
