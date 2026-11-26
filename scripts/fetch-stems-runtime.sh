#!/bin/sh
# Fetches the official ONNX Runtime release (MIT) into third_party/onnxruntime (git-ignored) and verifies its SHA-256.
# Usage: scripts/fetch-stems-runtime.sh [osx-arm64|osx-x86_64|linux-x64|linux-aarch64|win-x64]
# Pinned hashes live in scripts/stems-runtime.sha256 ("<sha256>  <asset file name>"). A download whose hash is not pinned
# or does not match is refused and removed; it is never unpacked.
set -eu
VER="${ORT_VERSION:-1.32.0}"
PLAT="${1:-}"
if [ -z "$PLAT" ]; then
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) PLAT=osx-arm64 ;; Darwin-x86_64) PLAT=osx-x86_64 ;;
    Linux-x86_64) PLAT=linux-x64 ;; Linux-aarch64) PLAT=linux-aarch64 ;;
    *) echo "unknown platform; pass one explicitly" >&2; exit 2 ;;
  esac
fi
EXT=tgz; [ "$PLAT" = win-x64 ] && EXT=zip
ASSET="onnxruntime-$PLAT-$VER.$EXT"
URL="https://github.com/microsoft/onnxruntime/releases/download/v$VER/$ASSET"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/onnxruntime"
PINS="$ROOT/scripts/stems-runtime.sha256"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
echo "downloading $URL"
curl -fL --proto '=https' --tlsv1.2 -o "$TMP/$ASSET" "$URL" || { echo "download failed (is the release published for $PLAT?)" >&2; exit 1; }
GOT="$(shasum -a 256 "$TMP/$ASSET" | cut -d' ' -f1)"
WANT="$(grep -F "  $ASSET" "$PINS" 2>/dev/null | cut -d' ' -f1 || true)"
if [ -z "$WANT" ]; then echo "REFUSED: no pinned SHA-256 for $ASSET in scripts/stems-runtime.sha256 (downloaded file hashes to $GOT)" >&2; exit 3; fi
if [ "$GOT" != "$WANT" ]; then echo "REFUSED: SHA-256 mismatch for $ASSET: expected $WANT, got $GOT" >&2; exit 3; fi
rm -rf "$DEST"; mkdir -p "$DEST"
case "$EXT" in tgz) tar -xzf "$TMP/$ASSET" -C "$DEST" --strip-components=1 ;; zip) unzip -q "$TMP/$ASSET" -d "$TMP/x" && mv "$TMP/x"/*/* "$DEST"/ ;; esac
echo "installed ONNX Runtime $VER ($PLAT) into $DEST"
echo "next: cmake -S . -B build -DDECKHAND_WITH_ONNX=ON   (auto-detected when the folder exists)"
