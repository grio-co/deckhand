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

# No sound card in a container: give ALSA a null device so the engine can open its output.
if [ ! -e /proc/asound/cards ] && [ ! -f "$HOME/.asoundrc" ]; then
  printf 'pcm.!default { type plug\n  slave.pcm { type null } }\nctl.!default { type null }\n' > "$HOME/.asoundrc"
fi

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
echo "== input: click the search box, type, click the language button (xdotool into Xvfb)"
xvfb-run -a -s "-screen 0 1600x1000x24" sh -c '
  "$1/deckhand" --deck-a "$2/media/test.flac" --screenshot "$2/shell-input.png" --frames 330 &
  pid=$!
  sleep 3
  xdotool mousemove 370 418 click 1
  sleep 0.4
  xdotool type --delay 100 "Tést"
  sleep 0.4
  xdotool mousemove 1200 16 click 1
  wait $pid' sh "$BUILD" "$OUT"
ls -l "$OUT"/*.png
tail -n 25 "$XDG_DATA_HOME/DeckHand/deckhand.log" || true
