#!/bin/sh
# Builds the DeckHandKit Swift package so another macOS app (Rig) can embed DeckHand.
# Usage: scripts/build-embed.sh   ->  platform/macos/DeckHandKit (add it as a local package dependency)
# It also leaves build/deckhand-engine, the helper process the host should ship (see DeckHandView.Configuration).
# No signing here: the host app signs everything it ships, including the helper (hardened runtime, no entitlements).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
PKG="$ROOT/platform/macos/DeckHandKit"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
LOG="$BUILD/build.log"
cmake --build "$BUILD" -j >"$LOG" 2>&1 || { grep -E "error" -A3 "$LOG" | head -30 >&2; echo "core build failed ($LOG)" >&2; exit 1; }
mkdir -p "$BUILD/embed"
# One archive with every core library, so the package needs a single -l.
libtool -static -o "$BUILD/embed/libdeckhand_core.a" \
  "$BUILD"/libdh_app.a "$BUILD"/libdh_lighting.a "$BUILD"/libdh_plugins.a "$BUILD"/libdh_stems.a "$BUILD"/libdh_sync.a "$BUILD"/libdh_services.a "$BUILD"/libdh_broadcast.a "$BUILD"/libdh_api.a \
  "$BUILD"/libdh_skin.a "$BUILD"/libdh_i18n.a "$BUILD"/libdh_export.a "$BUILD"/libdh_capture.a "$BUILD"/libdh_analysis.a "$BUILD"/libdh_midi.a \
  "$BUILD"/libdh_import.a "$BUILD"/libdh_library.a "$BUILD"/libdh_audio.a "$BUILD"/libdh_gfx.a "$BUILD"/libdh_log.a \
  "$BUILD"/libdh_platform.a "$BUILD"/libdh_miniaudio.a 2>/dev/null
# tls_link.txt and sqlite_link.txt hold absolute .a paths and flags: pass them through as separate flags.
FLAGS=$( (cat "$BUILD/tls_link.txt"; echo; cat "$BUILD/sqlite_link.txt"; echo) | tr ' ' '\n' | grep -v '^$' | sed 's/.*/"&"/' | paste -sd, -)
sed -e "s|@LIBDIR@|$BUILD/embed|" -e "s|@LINKFLAGS@|[$FLAGS]|" "$PKG/Package.swift.in" > "$PKG/Package.swift"
(cd "$PKG" && swift build -c release >"$BUILD/embed/swift.log" 2>&1) || { grep -E "error:" "$BUILD/embed/swift.log" | head -10 >&2; exit 1; }
echo "embed ready: $PKG  (helper: $BUILD/deckhand-engine, resources: $ROOT/resources + controllers/*.dhmap)"
