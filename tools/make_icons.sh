#!/bin/sh
# Regenerates every platform icon asset from the masters in design/icon/ (deterministic; no network, nothing to install).
# Needs macOS (swiftc, iconutil, sips) and python3 (standard library only).
#   tools/make_icons.sh          rebuild all assets from the committed SVG masters
#   tools/make_icons.sh --svg    also regenerate the masters and concept SVGs first (design/icon/src/gen_svg.py)
# Masters: deckhand-icon.svg (full-bleed square), deckhand-mark.svg (free-form, transparent), and the *-small.svg optical
# variants used for 16-48 px (no grooves, heavier strokes). See design/icon/README.md.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D="$ROOT/design/icon"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
[ "${1:-}" = "--svg" ] && python3 "$D/src/gen_svg.py"

swiftc -O "$ROOT/tools/icon_render.swift" -o "$T/ir" 2>&1 | grep -E "error" && exit 1
IR="$T/ir"
ICON="$D/deckhand-icon.svg";   ICON_S="$D/deckhand-icon-small.svg"
MARK="$D/deckhand-mark.svg";   MARK_S="$D/deckhand-mark-small.svg"
pick() { if [ "$1" -le 48 ]; then echo "$2"; else echo "$3"; fi; }   # pick SIZE SMALL FULL

# ---- design/icon: 512 px preview of the primary (as it appears in the Dock)
"$IR" tile "$ICON" "$D/deckhand-icon-512.png" 512

# ---- macOS: .icns from an .iconset (rounded tile, margins, shadow; small sizes use the small variant)
IS="$T/DeckHand.iconset"; mkdir -p "$IS"
for spec in 16:icon_16x16 32:icon_16x16@2x 32:icon_32x32 64:icon_32x32@2x 128:icon_128x128 256:icon_128x128@2x \
            256:icon_256x256 512:icon_256x256@2x 512:icon_512x512 1024:icon_512x512@2x; do
  s=${spec%%:*}; n=${spec#*:}
  "$IR" tile "$(pick "$s" "$ICON_S" "$ICON")" "$IS/$n.png" "$s"
done
iconutil -c icns "$IS" -o "$ROOT/platform/macos/DeckHand.icns"

# ---- iOS: full-bleed 1024 px, opaque, no rounded corners (the system masks them): light, dark, tinted
IOS="$D/ios/AppIcon.appiconset"; mkdir -p "$IOS"
"$IR" flat "$ICON" "$IOS/AppIcon-1024.png" 1024
"$IR" dark "$MARK" "$IOS/AppIcon-1024-dark.png" 1024
"$IR" tint "$ICON" "$IOS/AppIcon-1024-tinted.png" 1024
cat > "$IOS/Contents.json" <<'JSON'
{
  "images" : [
    {
      "filename" : "AppIcon-1024.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "appearances" : [
        {
          "appearance" : "luminosity",
          "value" : "dark"
        }
      ],
      "filename" : "AppIcon-1024-dark.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "appearances" : [
        {
          "appearance" : "luminosity",
          "value" : "tinted"
        }
      ],
      "filename" : "AppIcon-1024-tinted.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
JSON

# ---- Windows: multi-size .ico from the free-form mark (PNG-compressed 256 entry)
SPECS=""
for s in 16 20 24 32 40 48 64 128 256; do
  "$IR" rgba "$(pick "$s" "$MARK_S" "$MARK")" "$T/w$s.rgba" "$s"
  SPECS="$SPECS $s:$T/w$s.rgba"
done
# shellcheck disable=SC2086
python3 "$ROOT/tools/icon_pack.py" ico "$ROOT/platform/windows/resources/deckhand.ico" $SPECS

# ---- Linux: scalable SVG, hicolor PNGs, and the raw pixels compiled into the SDL shell for SDL_SetWindowIcon
PK="$ROOT/platform/linux/packaging"
cp "$MARK" "$PK/deckhand.svg"
for s in 16 24 32 48 64 128 256 512; do
  mkdir -p "$PK/icons/hicolor/${s}x${s}/apps"
  "$IR" svg "$(pick "$s" "$MARK_S" "$MARK")" "$PK/icons/hicolor/${s}x${s}/apps/deckhand.png" "$s"
done
"$IR" rgba "$MARK" "$T/win48.rgba" 48
python3 "$ROOT/tools/icon_pack.py" header "$T/win48.rgba" "$ROOT/platform/linux/shell/window_icon.hpp" 48

# ---- web: 32 px favicon as base64 (pasted into src/broadcast/overlay_page.cpp)
"$IR" svg "$MARK_S" "$T/fav.png" 32
base64 < "$T/fav.png" | tr -d '\n' > "$D/favicon.b64"; echo >> "$D/favicon.b64"
echo "icons regenerated from $D"
