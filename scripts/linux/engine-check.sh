#!/bin/sh
# Runs inside the build container (or a Linux box with Xvfb, ffmpeg, curl, xdotool): verifies the engine helper
# process on Linux with a null audio output. Plays a track through the HTTP API, kill -9s the UI, and checks the
# helper keeps playing, a new UI re-attaches, a normal quit stops the helper and a killed helper is restarted.
# Usage: scripts/linux/engine-check.sh BUILD_DIR OUT_DIR
set -u
BUILD=${1:?build dir}; OUT=${2:?output dir}
mkdir -p "$OUT/engine"
export XDG_DATA_HOME="$OUT/engine/data" DECKHAND_LOG=info LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8
export DECKHAND_ENGINE_PROCESS=${DECKHAND_ENGINE_PROCESS:-separate}
rm -rf "$XDG_DATA_HOME"
[ -n "${XDG_RUNTIME_DIR:-}" ] || { XDG_RUNTIME_DIR=$(mktemp -d); chmod 700 "$XDG_RUNTIME_DIR"; export XDG_RUNTIME_DIR; }
# No sound device on purpose: with an empty ALSA configuration and no sound server the engine reports "No audio
# output" and runs on miniaudio's null backend, which is paced like a real device. (An ALSA "null" PCM is not
# paced: it would play the whole track in a blink.)
: > "$OUT/engine/empty-alsa.conf"
export ALSA_CONFIG_PATH="$OUT/engine/empty-alsa.conf" PULSE_SERVER=unix:/nonexistent/dh-pulse
ffmpeg -v error -y -f lavfi -i "sine=frequency=220:duration=240:sample_rate=44100" "$OUT/engine/long.flac"
DATA="$XDG_DATA_HOME/DeckHand"
FAIL=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; FAIL=1; }

Xvfb :77 -screen 0 1600x1000x24 >/dev/null 2>&1 & XVFB=$!
export DISPLAY=:77
sleep 1
api() { curl -s -m 3 -H "Authorization: Bearer $TOKEN" "$@"; }
# live helper only: a killed helper whose parent never reaps it stays a zombie and still shows in pgrep
engine_pid() { ps -eo pid=,stat=,comm= | awk '$3 == "deckhand-engine" && $2 !~ /^Z/ { print $1; exit }'; }
pos() { api http://127.0.0.1:7373/v1/decks/1 | sed -n 's/.*"position[^:]*:\([0-9.]*\).*/\1/p' | head -1; }
wait_api() { i=0; while [ $i -lt 100 ]; do [ -f "$DATA/api-admin-token.txt" ] && TOKEN=$(cat "$DATA/api-admin-token.txt") && api http://127.0.0.1:7373/v1/status | grep -q . && return 0; sleep 0.3; i=$((i+1)); done; return 1; }

start_ui() { "$BUILD/deckhand" "$@" >"$OUT/engine/ui.out" 2>&1 & UI=$!; }

echo "== 1. start UI with a track, mute, play"
start_ui --deck-a "$OUT/engine/long.flac"
wait_api && ok "API up" || { bad "API did not come up"; tail -20 "$DATA/deckhand.log"; }
sleep 3
EP=$(engine_pid); [ -n "$EP" ] && ok "helper running (pid $EP)" || bad "no deckhand-engine process"
api -X POST -d '{"value":0}' http://127.0.0.1:7373/v1/mixer/master_level >/dev/null
api -X POST http://127.0.0.1:7373/v1/decks/1/play >/dev/null
sleep 2
P1=$(pos); sleep 2; P2=$(pos)
echo "position $P1 -> $P2"
[ -n "$P1" ] && awk "BEGIN{exit !($P2 > $P1 + 0.5)}" && ok "deck 1 advancing" || { bad "deck not advancing"; api http://127.0.0.1:7373/v1/decks/1; }
ls /dev/shm 2>/dev/null | sed 's/^/  shm: /'; ls "$XDG_RUNTIME_DIR/deckhand" 2>/dev/null | sed 's/^/  runtime: /'

echo "== 2. kill -9 the UI"
T0=$(date +%s.%N)
kill -9 $UI; wait $UI 2>/dev/null
sleep 3
EP2=$(engine_pid)
[ "$EP2" = "$EP" ] && ok "helper survived (pid $EP2)" || bad "helper gone or changed ($EP -> $EP2)"
echo "== 3. relaunch UI; position keeps advancing"
start_ui
wait_api && ok "API up again" || bad "API did not come back"
T1=$(date +%s.%N)
sleep 2
P3=$(pos); sleep 2; P4=$(pos)
echo "position after relaunch $P3 -> $P4 (UI was dead ~$(awk "BEGIN{printf \"%.1f\", $T1-$T0}") s, position before kill $P2)"
awk "BEGIN{exit !($P3 > $P2 + 4)}" && ok "position continued while the UI was dead (+$(awk "BEGIN{printf \"%.1f\", $P3-$P2}") s)" || bad "position did not continue"
awk "BEGIN{exit !($P4 > $P3 + 0.5)}" && ok "still advancing after re-attach" || bad "not advancing after re-attach"
[ "$(engine_pid)" = "$EP" ] && ok "same helper after re-attach" || bad "helper changed"

echo "== 4. helper killed -> restarted"
kill -9 "$EP"; i=0; while [ $i -lt 40 ]; do sleep 0.5; N=$(engine_pid); [ -n "$N" ] && [ "$N" != "$EP" ] && break; i=$((i+1)); done
[ -n "${N:-}" ] && [ "$N" != "$EP" ] && ok "helper restarted (pid $N) after $(awk "BEGIN{print $i*0.5}") s" || bad "helper not restarted"
kill -0 $UI 2>/dev/null && ok "UI still alive" || bad "UI died with the helper"

echo "== 5. normal quit stops the helper"
kill -TERM $UI; i=0; while [ $i -lt 30 ] && kill -0 $UI 2>/dev/null; do sleep 0.3; i=$((i+1)); done
sleep 1.5
[ -z "$(engine_pid)" ] && ok "helper stopped after quit" || bad "helper still running after quit"
ls /dev/shm 2>/dev/null | grep -c '^dh' | sed 's/^/  leftover shm segments: /'
kill $XVFB 2>/dev/null
cp "$DATA"/deckhand*.log "$OUT/engine/" 2>/dev/null
[ $FAIL = 0 ] && echo "ENGINE CHECK PASSED" || echo "ENGINE CHECK FAILED"
exit $FAIL
