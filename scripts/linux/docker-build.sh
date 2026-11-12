#!/bin/sh
# Builds the Linux image, copies the source tree into a persistent build container, then builds, runs the
# tests and the shell smoke test there. The tree is copied (not bind-mounted) so this also works when the
# Docker daemon runs in a VM or on another host.
#
# Usage: scripts/linux/docker-build.sh [--rebuild-image] [--no-smoke]
#   DECKHAND_SRC   tree to build (default: this repository)
#   DECKHAND_OUT   where artefacts (shell.png, logs) are copied (default: build-linux-artifacts/ in the repo)
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${DECKHAND_SRC:-$ROOT}
OUT=${DECKHAND_OUT:-$ROOT/build-linux-artifacts}
IMAGE=deckhand-linux-build
CONTAINER=deckhand-linux-build
SMOKE=1
REBUILD=0
for a in "$@"; do
  case $a in
    --rebuild-image) REBUILD=1 ;;
    --no-smoke) SMOKE=0 ;;
    *) echo "usage: $0 [--rebuild-image] [--no-smoke]" >&2; exit 2 ;;
  esac
done

if [ "$REBUILD" = 1 ] || ! docker image inspect $IMAGE >/dev/null 2>&1; then
  docker build -t $IMAGE "$ROOT/scripts/linux"
  docker rm -f $CONTAINER >/dev/null 2>&1 || true
fi
if ! docker container inspect $CONTAINER >/dev/null 2>&1; then
  # /dev/snd gives the MIDI test a real ALSA sequencer when the Docker host has one; optional.
  docker rm -f $CONTAINER >/dev/null 2>&1 || true
  docker run -d --name $CONTAINER --device /dev/snd $IMAGE sleep infinity >/dev/null 2>&1 ||
    docker run -d --name $CONTAINER $IMAGE sleep infinity >/dev/null
fi
docker start $CONTAINER >/dev/null

# Source goes to /work/src, build output stays in the container at /work/build (kept between runs).
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rsync -a --exclude='/.git' --exclude='/build*' --exclude='*.o' "$SRC"/ "$TMP"/
COPYFILE_DISABLE=1 tar -C "$TMP" -cf - . |
  docker exec -i $CONTAINER sh -c 'rm -rf /work/src && mkdir -p /work/src && tar -x -C /work/src 2>/dev/null'

docker exec -e BUILD_DIR=/work/build $CONTAINER sh -c 'cd /work/src && scripts/build-linux.sh --test'
if [ "$SMOKE" = 1 ]; then
  docker exec $CONTAINER sh -c 'rm -rf /work/out && /work/src/scripts/linux/smoke.sh /work/build /work/out'
  mkdir -p "$OUT"
  docker cp $CONTAINER:/work/out/. "$OUT/"
  echo "artefacts in $OUT"
fi
