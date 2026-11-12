#!/bin/sh
# Cross-builds the Windows port with mingw-w64 inside a container, runs the tests under Wine and collects the
# artefacts. The tree is copied into the container (not bind-mounted), so this also works with a remote or VM Docker.
#
# Usage: scripts/windows/docker-build.sh [--rebuild-image] [--no-tests] [--no-shot]
#   DECKHAND_SRC   tree to build (default: this repository)
#   DECKHAND_OUT   artefacts (exes, test logs, shell.png, sha256) land here (default: build-windows-artifacts/)
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${DECKHAND_SRC:-$ROOT}
OUT=${DECKHAND_OUT:-$ROOT/build-windows-artifacts}
IMAGE=deckhand-windows-build
CONTAINER=deckhand-windows-build
TESTS=1
SHOT=1
REBUILD=0
for a in "$@"; do
  case $a in
    --rebuild-image) REBUILD=1 ;;
    --no-tests) TESTS=0 ;;
    --no-shot) SHOT=0 ;;
    *) echo "usage: $0 [--rebuild-image] [--no-tests] [--no-shot]" >&2; exit 2 ;;
  esac
done

if [ "$REBUILD" = 1 ] || ! docker image inspect $IMAGE >/dev/null 2>&1; then
  docker build -t $IMAGE "$ROOT/scripts/windows"
  docker rm -f $CONTAINER >/dev/null 2>&1 || true
fi
# A container made from an older image would keep the old toolchain: recreate it when the image changed.
if docker container inspect $CONTAINER >/dev/null 2>&1 &&
   [ "$(docker container inspect -f '{{.Image}}' $CONTAINER)" != "$(docker image inspect -f '{{.Id}}' $IMAGE)" ]; then
  docker rm -f $CONTAINER >/dev/null
fi
docker container inspect $CONTAINER >/dev/null 2>&1 || docker run -d --name $CONTAINER $IMAGE sleep infinity >/dev/null
docker start $CONTAINER >/dev/null

# Source goes to /work/src; build output stays in the container at /work/build (kept between runs).
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rsync -a --exclude='/.git' --exclude='/build*' --exclude='*.o' "$SRC"/ "$TMP"/
COPYFILE_DISABLE=1 tar -C "$TMP" -cf - . |
  docker exec -i $CONTAINER sh -c 'rm -rf /work/src && mkdir -p /work/src && tar -x -C /work/src 2>/dev/null'

rc=0
docker exec -e TESTS=$TESTS -e SHOT=$SHOT $CONTAINER sh /work/src/scripts/windows/container-build.sh || rc=$?

rm -rf "$OUT"
mkdir -p "$OUT"
docker cp $CONTAINER:/work/out/. "$OUT/" 2>/dev/null || true
echo "artefacts in $OUT"
exit $rc
