#!/bin/sh
# Builds DeckHand.app for macOS and signs it. Usage: scripts/build-macos.sh
#
# Signing identity (never ad-hoc): set DECKHAND_SIGN_IDENTITY, or put the identity name on
# the first line of .signing.local in the repository root (that file is not committed).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
APP="$BUILD/DeckHand.app"
BUNDLE_ID=${DECKHAND_BUNDLE_ID:-org.deckhand.DeckHand}
VERSION=$(sed -n 's/^project(DeckHand VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")

IDENTITY=${DECKHAND_SIGN_IDENTITY:-}
if [ -z "$IDENTITY" ] && [ -f "$ROOT/.signing.local" ]; then
  IDENTITY=$(sed -n '1p' "$ROOT/.signing.local")
fi
if [ -z "$IDENTITY" ]; then
  echo "error: no signing identity. Set DECKHAND_SIGN_IDENTITY or create .signing.local" >&2
  exit 1
fi

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
# Stop on any compile error: never bundle a stale library.
LOG="$BUILD/build.log"
if ! cmake --build "$BUILD" -j >"$LOG" 2>&1; then
  grep -E "error|Error " -A3 "$LOG" | head -40 >&2
  echo "error: core build failed (full log: $LOG)" >&2
  exit 1
fi
grep -E "warning: [^i]" "$LOG" || true
[ -f "$BUILD/libdh_app.a" ] || { echo "error: core build failed" >&2; exit 1; }
[ -x "$BUILD/deckhand-engine" ] || { echo "error: engine helper build failed" >&2; exit 1; }

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/controllers"

# shellcheck disable=SC2046
swiftc -O -swift-version 5 \
  -import-objc-header "$ROOT/platform/macos/Shell/Bridging.h" -I "$ROOT/src/app/include" \
  "$ROOT"/platform/macos/Shell/*.swift \
  -o "$APP/Contents/MacOS/DeckHand" \
  -L "$BUILD" -ldh_app -ldh_sync -ldh_services -ldh_broadcast -ldh_api -ldh_skin -ldh_i18n -ldh_export -ldh_analysis -ldh_midi -ldh_import \
  -ldh_library -ldh_audio -ldh_gfx -ldh_log -ldh_platform -ldh_miniaudio \
  $(cat "$BUILD/tls_link.txt") $(cat "$BUILD/sqlite_link.txt") -lc++ \
  -framework AppKit -framework Metal -framework MetalKit -framework QuartzCore -framework CoreText \
  -framework CoreMIDI -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

cp "$ROOT"/controllers/*.dhmap "$APP/Contents/Resources/controllers/"
cp -R "$ROOT/resources/lang" "$ROOT/resources/skins" "$APP/Contents/Resources/"
sed -e "s/@BUNDLE_ID@/$BUNDLE_ID/" -e "s/@VERSION@/$VERSION/g" \
    -e "s/@MIN_OS@/$(sw_vers -productVersion | cut -d. -f1).0/" \
    "$ROOT/platform/macos/Info.plist" > "$APP/Contents/Info.plist"

# The audio engine runs as a helper process beside the app (see ARCHITECTURE.md). It is signed first, with the same
# identity and hardened runtime and no entitlements: shared memory and Unix sockets between the two processes need none.
cp "$BUILD/deckhand-engine" "$APP/Contents/MacOS/deckhand-engine"
codesign --force --options runtime --timestamp --identifier "$BUNDLE_ID.engine" --sign "$IDENTITY" "$APP/Contents/MacOS/deckhand-engine"
codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
codesign --verify --strict "$APP"
codesign --verify --strict "$APP/Contents/MacOS/deckhand-engine"
echo "built and signed: $APP"
