#!/bin/sh
# Runs inside the build container (or any Linux box with Xvfb and ffmpeg): decoder checks on real codecs,
# a text sheet, a command-line smoke test and software-GL screenshots of the desktop shell with tracks loaded.
# Usage: scripts/linux/smoke.sh BUILD_DIR OUT_DIR
set -eu
BUILD=${1:?build dir}
OUT=${2:?output dir}
mkdir -p "$OUT/media"
export XDG_DATA_HOME="$OUT/data"
export DECKHAND_LOG=${DECKHAND_LOG:-debug}
export LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8
rm -rf "$XDG_DATA_HOME"

# No sound card in a container: an empty ALSA configuration and no sound server make the engine report
# "No audio output" and run on miniaudio's paced null backend (the fallback in src/audio/engine.cpp).
: > "$OUT/empty-alsa.conf"
export ALSA_CONFIG_PATH="$OUT/empty-alsa.conf" PULSE_SERVER=unix:/nonexistent/dh-pulse
export DECKHAND_ENGINE_PROCESS=${DECKHAND_ENGINE_PROCESS:-separate}

echo "== media (3 s, 120 BPM kick pattern) in every format FFmpeg is asked to read"
SRC="aevalsrc='0.8*sin(2*PI*55*t)*pow(max(0,sin(2*PI*2*t)),6)|0.6*sin(2*PI*110*t)*pow(max(0,sin(2*PI*2*t)),6)':d=3:s=44100"
for ext in mp3 m4a flac ogg opus wav aiff; do
  ffmpeg -v error -y -f lavfi -i "$SRC" -metadata "title=$(printf 'T\303\251st \342\230\203')" \
    -metadata "artist=$(printf '\303\204rtist')" -metadata "album=$(printf 'Alb\303\274m')" \
    $( [ "$ext" = opus ] && echo "-ar 48000" ) "$OUT/media/test.$ext"
done
ls -l "$OUT/media"
DECKHAND_TEST_MEDIA="$OUT/media" "$BUILD/test_linux_decoder"
"$BUILD/test_linux_text" "$OUT/text.png"

echo "== deckhandctl"
"$BUILD/deckhandctl" 2>&1 | head -20 || true

echo "== shell under Xvfb (Mesa llvmpipe): empty, then with two tracks loaded"
run_shell() { xvfb-run -a -s "-screen 0 1600x1000x24" "$BUILD/deckhand" "$@"; }
run_shell --screenshot "$OUT/shell.png" --frames 40
run_shell --deck-a "$OUT/media/test.flac" --deck-b "$OUT/media/test.mp3" --screenshot "$OUT/shell-tracks.png" --frames 150
echo "== input: click the search box, type, click the 2 / 4 decks button (xdotool into Xvfb)"
xvfb-run -a -s "-screen 0 1600x1000x24" sh -c '
  "$1/deckhand" --deck-a "$2/media/test.flac" --screenshot "$2/shell-input.png" --frames 330 &
  pid=$!
  sleep 3
  win=$(xdotool search --sync --name DeckHand | head -1)
  xdotool mousemove --window "$win" 350 611 click 1
  sleep 0.4
  xdotool type --delay 100 "st"
  sleep 0.4
  xdotool mousemove --window "$win" 780 20 click 1
  wait $pid' sh "$BUILD" "$OUT"
ls -l "$OUT"/*.png
tail -n 25 "$XDG_DATA_HOME/DeckHand/deckhand.log" || true

echo "== skins, layouts and languages (settings written through the API, then a screenshot per combination)"
DATA="$XDG_DATA_HOME/DeckHand"
shot() {  # shot NAME key=value ...   (settings are read at start-up)
  name=$1; shift
  if [ $# -gt 0 ]; then
    xvfb-run -a -s "-screen 0 1600x1000x24" sh -c '
      "$1/deckhand" >/dev/null 2>&1 &
      pid=$!
      i=0; while [ $i -lt 100 ] && ! [ -f "$2/api-admin-token.txt" ]; do sleep 0.3; i=$((i+1)); done
      sleep 1.5
      tok=$(cat "$2/api-admin-token.txt"); shift 2
      for kv in "$@"; do
        curl -s -m 3 -X PUT -H "Authorization: Bearer $tok" -d "{\"value\":\"${kv#*=}\"}" "http://127.0.0.1:7373/v1/settings/${kv%%=*}" >/dev/null
      done
      kill -TERM $pid; wait $pid || true' sh "$BUILD" "$DATA" "$@"
  fi
  run_shell --deck-a "$OUT/media/test.flac" --deck-b "$OUT/media/test.mp3" --screenshot "$OUT/$name.png" --frames 150
}
shot skin-default-two ui.skin=default ui.layout=main ui.language=en
shot skin-default-four ui.layout=four
if [ -d "$BUILD/share/deckhand/skins/voidwave.dhskin" ] || [ -f "$BUILD/share/deckhand/skins/voidwave.dhskin" ]; then
  shot skin-example ui.skin=voidwave ui.layout=main
fi
shot skin-german ui.skin=default ui.layout=main ui.language=de
shot skin-restore ui.skin=default ui.layout=main ui.language=en
pkill -x deckhand-engine 2>/dev/null || true
ls -l "$OUT"/*.png
