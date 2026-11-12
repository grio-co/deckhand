#!/bin/sh
# Builds DeckHand for Linux (core, deckhandctl, the desktop shell and the tests).
# Usage: scripts/build-linux.sh [--test] [--install PREFIX] [--package]
#   --package     also write build-linux-artifacts/deckhand-linux-<arch>.tar.gz (scripts/linux/package.sh)
#   BUILD_DIR     build tree (default: build-linux in the repository)
#   CMAKE_ARGS    extra arguments for the configure step (for example -DDECKHAND_BUILD_SHELL=OFF)
# Dependencies per distribution are listed in docs/LINUX.md; scripts/linux/Dockerfile is a worked example.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/build-linux}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
RUN_TESTS=0
PREFIX=
PACKAGE=0

while [ $# -gt 0 ]; do
  case $1 in
    --test) RUN_TESTS=1 ;;
    --package) PACKAGE=1 ;;
    --install) shift; PREFIX=${1:?--install needs a prefix} ;;
    *) echo "usage: $0 [--test] [--install PREFIX] [--package]" >&2; exit 2 ;;
  esac
  shift
done

GENERATOR=
command -v ninja >/dev/null 2>&1 && GENERATOR="-G Ninja"

# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD" $GENERATOR -DCMAKE_BUILD_TYPE=Release ${CMAKE_ARGS:-}
cmake --build "$BUILD" -j "$JOBS"

if [ "$RUN_TESTS" = 1 ]; then
  (cd "$BUILD" && ctest --output-on-failure -j "$JOBS")
fi
if [ -n "$PREFIX" ]; then
  cmake --install "$BUILD" --prefix "$PREFIX"
fi
if [ "$PACKAGE" = 1 ]; then
  "$ROOT/scripts/linux/package.sh" "$BUILD" "${OUT_DIR:-$ROOT/build-linux-artifacts}"
fi
echo "built: $BUILD/deckhand (with deckhand-engine beside it)"
