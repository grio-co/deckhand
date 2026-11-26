#!/bin/sh
# Regenerates third_party/sqlcipher/{sqlite3.c,sqlite3.h,LICENSE.md,SHA256SUMS} from the pinned SQLCipher release.
#
#   scripts/vendor-sqlcipher.sh                    # download the pinned release, verify its SHA-256, generate
#   SQLCIPHER_TARBALL=/path/v4.20.0.tar.gz scripts/vendor-sqlcipher.sh   # use a tarball you already have
#
# To update: change VERSION and TARBALL_SHA256 below (take the hash from a download you trust, not from this script),
# re-run, review the diff, update third_party/sqlcipher/README.md. Needs curl or a local tarball, tar, make and a C
# compiler; the SQLite build in the tarball brings its own TCL interpreter (jimsh), so tclsh is not required.
set -eu

VERSION=4.20.0
TARBALL_SHA256=38705f51b61ac0636a5fd064f9a77fafabc7227b2aa2513f1b14c8bb1102affc
URL="https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v${VERSION}.tar.gz"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/third_party/sqlcipher"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dh-sqlcipher.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }

TARBALL=${SQLCIPHER_TARBALL:-}
if [ -z "$TARBALL" ]; then
  TARBALL="$WORK/sqlcipher.tar.gz"
  curl -fsSL -o "$TARBALL" "$URL"
fi
GOT=$(sha256 "$TARBALL")
if [ "$GOT" != "$TARBALL_SHA256" ]; then
  echo "error: SHA-256 of the SQLCipher tarball is $GOT, expected $TARBALL_SHA256" >&2
  exit 1
fi

mkdir "$WORK/src"
tar -xf "$TARBALL" -C "$WORK/src" --strip-components 1
cd "$WORK/src"
# The flags only steer configure's feature checks; the real compile flags are set by CMakeLists.txt.
./configure --disable-tcl --with-tempstore=yes CFLAGS="-DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2" >"$WORK/configure.log" 2>&1 \
  || { tail -20 "$WORK/configure.log" >&2; exit 1; }
make sqlite3.c >"$WORK/make.log" 2>&1 || { tail -20 "$WORK/make.log" >&2; exit 1; }

mkdir -p "$DEST"
cp sqlite3.c sqlite3.h LICENSE.md "$DEST/"
( cd "$DEST" && for f in sqlite3.c sqlite3.h LICENSE.md; do echo "$(sha256 $f)  $f"; done > SHA256SUMS )
echo "SQLCipher $VERSION (SQLite $(sed -n 's/^#define SQLITE_VERSION *"\(.*\)"/\1/p' sqlite3.h)) vendored into $DEST"
cat "$DEST/SHA256SUMS"
