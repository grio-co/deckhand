#!/bin/sh
# Makes the portable tarball deckhand-linux-<arch>.tar.gz from a finished build tree (scripts/build-linux.sh).
# Layout: bin/{deckhand,deckhand-engine,deckhandctl}, share/deckhand/{controllers,lang,skins}, share/applications,
# share/icons, README-linux.txt, LICENSE. It is NOT a static or AppImage build: it needs the distribution's shared
# libraries (docs/LINUX.md, "Packaging"); the script prints what the binaries link against.
# Usage: scripts/linux/package.sh [BUILD_DIR [OUT_DIR]]   (defaults: build-linux, build-linux-artifacts)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-${BUILD_DIR:-$ROOT/build-linux}}
OUT=${2:-$ROOT/build-linux-artifacts}
ARCH=$(uname -m)
NAME=deckhand-linux-$ARCH
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$OUT"
cmake --install "$BUILD" --prefix "$STAGE/$NAME" >/dev/null
for f in bin/deckhand bin/deckhand-engine bin/deckhandctl; do
  [ -x "$STAGE/$NAME/$f" ] || { echo "package: $f missing from the install" >&2; exit 1; }
done
strip --strip-unneeded "$STAGE/$NAME"/bin/* 2>/dev/null || true
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$STAGE/$NAME/"
cat > "$STAGE/$NAME/README-linux.txt" <<'README'
DeckHand for Linux (portable archive)
=====================================

Run:      ./bin/deckhand [FILE...]
Command line client: ./bin/deckhandctl
The audio engine runs as ./bin/deckhand-engine, started by deckhand from its own folder (keep the three
programs together). Music keeps playing if the window crashes; see ARCHITECTURE.md in the source tree.

Portable mode: create an empty file named "portable" beside the programs:

    touch bin/portable

Then the library, settings and logs live in bin/DeckHandData/ instead of ~/.local/share/DeckHand, and the
folder can be copied to a USB stick. Without that file the data goes to $XDG_DATA_HOME/DeckHand.

This archive is not self-contained. It needs these from your distribution (names are Debian/Ubuntu's):
  libsdl3 (SDL3, 3.2 or newer)   libpango-1.0, libpangocairo-1.0, libcairo2
  libavformat, libavcodec, libavutil, libswresample (FFmpeg 6 or newer)
  libasound2 (ALSA)              libsqlcipher, libssl3, zlib1g
  fonts-noto-core (and fonts-noto-cjk for CJK text)
Audio output is found at run time: PulseAudio / PipeWire (pulse), ALSA or JACK, whichever is present.
The desktop entry and icon are in share/applications and share/icons; copy them under ~/.local/share to add
DeckHand to your launcher (edit Exec= to the full path of bin/deckhand).

DECKHAND_LOG=debug writes more detail to deckhand.log. Licences: see LICENSE and docs/LINUX.md.
README
echo "== shared libraries the programs link directly (readelf NEEDED; their own dependencies come with them)"
for b in deckhand deckhand-engine deckhandctl; do
  readelf -d "$STAGE/$NAME/bin/$b" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
done | sort -u | sed 's/^/  /'
tar -C "$STAGE" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "package: $OUT/$NAME.tar.gz"
