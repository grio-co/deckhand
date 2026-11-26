#!/bin/sh
# Runs inside the Windows build container (scripts/windows/container-build.sh; Wine and Xvfb are already set up): the
# real deckhand.exe with its window, driven through its REST API, against the separate engine process under Wine. The
# native equivalent for real Windows (and the CI job) is scripts/windows/engine-check.ps1; this one has the same steps:
# play, kill the UI, helper keeps playing, relaunch re-attaches with the position continued, killed helper restarts,
# orderly quit (WM_CLOSE through taskkill) stops the helper. Wine is only an approximation of Windows semantics.
# Usage: engine-check-wine.sh BUILD_DIR OUT_DIR
set -u
BUILD=${1:?build dir}; OUT=${2:?output dir}
W=$OUT/engine-wine
APP=$W/app
DATA=$APP/DeckHandData
rm -rf "$W"; mkdir -p "$APP"
cp "$BUILD/deckhand.exe" "$BUILD/deckhand-engine.exe" "$APP/"
cp -r "$BUILD/share" "$APP/share"
: > "$APP/portable"
export DECKHAND_LOG=info DECKHAND_ENGINE_PROCESS=separate
# a 90 s tone: one second of audio (integer cycles, two beats) repeated
perl -e 'my $r=44100; my $d=""; for my $i (0..$r-1){ my $t=$i/$r; my $beat=($t*2)-int($t*2); my $env=exp(-8*$beat);
  my $v=int(32767*0.5*($env*sin(6.28318*80*$t)+0.2*sin(6.28318*440*$t))); $d.=pack("s<s<",$v,$v);} $d = $d x 90;
  print "RIFF",pack("V",36+length $d),"WAVEfmt ",pack("VvvVVvv",16,1,2,$r,$r*4,4,16),"data",pack("V",length $d),$d;' > "$W/tone.wav"

FAIL=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; FAIL=1; }
TOKEN=
api() { curl -s -m 4 -H "Authorization: Bearer $TOKEN" "$@"; }
wait_api() { i=0; while [ $i -lt 200 ]; do [ -f "$DATA/api-admin-token.txt" ] && TOKEN=$(cat "$DATA/api-admin-token.txt") && api http://127.0.0.1:7373/v1/status | grep -q . && return 0; sleep 0.3; i=$((i+1)); done; return 1; }
pos() { api http://127.0.0.1:7373/v1/decks/1 | sed -n 's/.*"position_seconds":\([0-9.]*\).*/\1/p' | head -1; }
loaded() { api http://127.0.0.1:7373/v1/decks/1 | grep -q '"loaded":true'; }
engine_pids() { pgrep -f 'deckhand-engine\.exe' | tr '\n' ' ' | sed 's/ *$//'; }
ui_kill() { pkill -9 -f '[\\/]deckhand\.exe'; }
start_ui() { (cd "$APP" && wine64 "$APP/deckhand.exe" "$@" >"$W/ui.out" 2>&1 &); }
now() { date +%s.%N; }

echo "== 1. start UI with a track, mute, play"
start_ui --deck-a "Z:$W/tone.wav"
wait_api && ok "API up" || { bad "API did not come up"; tail -20 "$DATA/deckhand.log" 2>/dev/null; }
api -X POST -d '{"value":0}' http://127.0.0.1:7373/v1/mixer/master_level >/dev/null
i=0; while [ $i -lt 100 ] && ! loaded; do sleep 0.5; i=$((i+1)); done
loaded && ok "track loaded" || bad "track did not load"
EP=$(engine_pids); [ -n "$EP" ] && ok "engine process running (unix pid $EP)" || bad "no deckhand-engine process"
api -X POST -d '{"value":0}' http://127.0.0.1:7373/v1/mixer/master_level >/dev/null
api -X POST http://127.0.0.1:7373/v1/decks/1/play >/dev/null
sleep 2; P1=$(pos); sleep 2; P2=$(pos); T0=$(now)
echo "position $P1 -> $P2"
[ -n "$P1" ] && awk "BEGIN{exit !($P2 > $P1 + 0.5)}" && ok "deck 1 advancing" || bad "deck 1 not advancing"

echo "== 2. kill the UI"
ui_kill; sleep 4
EP2=$(engine_pids)
[ "$EP2" = "$EP" ] && ok "helper survived ($EP2)" || bad "helper gone or changed ($EP -> $EP2)"

echo "== 3. relaunch the UI"
start_ui
wait_api && ok "API up again" || bad "API did not come back"
i=0; while [ $i -lt 60 ] && ! loaded; do sleep 0.5; i=$((i+1)); done
P3=$(pos); T1=$(now)
GAP=$(awk "BEGIN{printf \"%.2f\", $T1-$T0}")
echo "position after relaunch $P3 (before the kill $P2, wall clock since then $GAP s)"
[ -n "$P3" ] && awk "BEGIN{d=($P3-$P2)-$GAP; exit !(d < 1.5 && d > -1.5)}" && ok "position continued while the UI was dead" || bad "position did not continue by the wall-clock gap"
sleep 2; P4=$(pos)
[ -n "$P4" ] && awk "BEGIN{exit !($P4 > $P3 + 0.5)}" && ok "still advancing after re-attach" || bad "not advancing after re-attach"
[ "$(engine_pids)" = "$EP" ] && ok "same helper after re-attach" || bad "helper changed"

echo "== 4. helper killed -> restarted"
kill -9 $EP
i=0; N=
while [ $i -lt 60 ]; do sleep 0.5; N=$(engine_pids); [ -n "$N" ] && [ "$N" != "$EP" ] && break; i=$((i+1)); done
[ -n "$N" ] && [ "$N" != "$EP" ] && ok "helper restarted ($N)" || bad "helper not restarted"
pgrep -f '[\\/]deckhand\.exe' >/dev/null && ok "UI still alive" || bad "UI died with the helper"

echo "== 5. orderly quit (taskkill without /F = WM_CLOSE)"
wine64 taskkill /IM deckhand.exe >/dev/null 2>&1
i=0; while [ $i -lt 60 ] && pgrep -f '[\\/]deckhand\.exe' >/dev/null; do sleep 0.5; i=$((i+1)); done
pgrep -f '[\\/]deckhand\.exe' >/dev/null && bad "UI did not exit after WM_CLOSE" || ok "UI exited"
i=0; while [ $i -lt 30 ] && [ -n "$(engine_pids)" ]; do sleep 0.5; i=$((i+1)); done
[ -z "$(engine_pids)" ] && ok "helper stopped after the quit" || bad "helper still running after the quit"
PROBE=$(wine64 "$APP/deckhand-engine.exe" --probe --data-dir "$(winepath -w "$DATA")" 2>/dev/null | tr -d '\r')
echo "probe: $PROBE"
echo "$PROBE" | grep -q "engine not running, main segment absent, 0 block segments, socket absent" && ok "no named objects left" || bad "objects left behind"

pkill -9 -f 'deckhand' 2>/dev/null
cp "$DATA"/deckhand*.log "$W/" 2>/dev/null
rm -rf "$DATA/api-"* 2>/dev/null   # throw-away API tokens
[ $FAIL = 0 ] && echo "ENGINE CHECK (WINE) PASSED" || echo "ENGINE CHECK (WINE) FAILED"
exit $FAIL
