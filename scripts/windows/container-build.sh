#!/bin/sh
# Runs inside the container (see docker-build.sh): configure, build, test under Wine, screenshot, collect.
set -u
SRC=/work/src
BUILD=/work/build
OUT=/work/out
rm -rf $OUT && mkdir -p $OUT

# ---- Wine ----------------------------------------------------------------------------------------
export WINEPREFIX=/root/.wine WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export DISPLAY=:99
# Wine maps non-ASCII file names through the Unix locale: without a UTF-8 one it cannot create "Müsik" or "東".
export LANG=C.UTF-8 LC_ALL=C.UTF-8
export PATH=$PATH:/usr/lib/wine
if ! pgrep Xvfb >/dev/null 2>&1; then
  Xvfb :99 -screen 0 1600x1000x24 +extension GLX +render -noreset >/tmp/xvfb.log 2>&1 &
  sleep 2
fi
cat > /usr/local/bin/wine-test <<'EOW'
#!/bin/sh
export PATH=$PATH:/usr/lib/wine
exec wine64 "$@"
EOW
chmod +x /usr/local/bin/wine-test
rm -rf $WINEPREFIX/drive_c/users/*/Temp/dh* $WINEPREFIX/drive_c/users/*/Temp/deckhand* 2>/dev/null
wine64 wineboot --init >/dev/null 2>&1
wineserver -w 2>/dev/null

# ---- configure and build (cross) --------------------------------------------------------------------
cmake -S $SRC -B $BUILD -G Ninja -DCMAKE_TOOLCHAIN_FILE=$SRC/cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CROSSCOMPILING_EMULATOR=/usr/local/bin/wine-test >$OUT/configure.log 2>&1
rc=$?
if [ $rc -ne 0 ]; then tail -30 $OUT/configure.log; echo "CONFIGURE FAILED"; exit 1; fi
cmake --build $BUILD >$OUT/build.log 2>&1
rc=$?
grep -E "warning:|error:" $OUT/build.log | sort | uniq -c | sort -rn | head -40
if [ $rc -ne 0 ]; then tail -40 $OUT/build.log; echo "BUILD FAILED"; exit 1; fi
echo "BUILD OK ($(grep -c 'warning:' $OUT/build.log) warnings)"

mkdir -p $OUT/bin
for f in deckhand deckhand-engine deckhandctl; do cp $BUILD/$f.exe $OUT/bin/; done
cp -r $BUILD/share $OUT/bin/ 2>/dev/null
(cd $OUT/bin && sha256sum *.exe > SHA256SUMS && cat SHA256SUMS)
ls -la $OUT/bin | head

# ---- tests under Wine ---------------------------------------------------------------------------------------
if [ "${TESTS:-1}" = 1 ]; then
  export DECKHAND_TEST_NO_MF=1 DECKHAND_TEST_LENIENT=1
  cd $BUILD
  : > $OUT/tests.txt
  for t in $(ctest -N | sed -n 's/^ *Test *#[0-9]*: //p'); do
    start=$(date +%s)
    ctest -R "^$t\$" --timeout 900 --output-on-failure >$OUT/test_$t.log 2>&1
    r=$?
    secs=$(( $(date +%s) - start ))
    if [ $r -eq 0 ]; then
      if grep -qi "skip" $OUT/test_$t.log; then res="PASS (with skips)"; else res=PASS; fi
    else res=FAIL; fi
    printf '%-26s %-18s %4ss\n' "$t" "$res" "$secs" | tee -a $OUT/tests.txt
  done
fi

# ---- shell screenshot (Xvfb + Wine: Direct3D 11 through wined3d / llvmpipe) ------------------------------------------
if [ "${SHOT:-1}" = 1 ]; then
  # a 12 s stereo test tone with a beat, for --deck-a
  perl -e 'my $r=44100; my $n=$r*12; my $d=""; for my $i (0..$n-1){ my $t=$i/$r; my $beat=($t*2)-int($t*2); my $env=exp(-8*$beat);
    my $v=int(32767*0.6*($env*sin(6.28318*80*$t)+0.2*sin(6.28318*440*$t))); $d.=pack("s<s<",$v,$v);}
    print "RIFF",pack("V",36+length $d),"WAVEfmt ",pack("VvvVVvv",16,1,2,$r,$r*4,4,16),"data",pack("V",length $d),$d;' > $OUT/tone.wav
  export DECKHAND_RESOURCE_DIR=$BUILD/share/deckhand
  mkdir -p $OUT/appdata && export APPDATA=$OUT/appdata
  for scale in 1 2; do
    DECKHAND_SCALE=$scale DECKHAND_LOG=debug timeout 180 wine64 $BUILD/deckhand.exe --screenshot "Z:$OUT/shell_x$scale.png" --frames 60 \
      --deck-a "Z:$OUT/tone.wav" --size 1440x900 >$OUT/shell_x$scale.log 2>&1
    echo "shell screenshot x$scale: exit $? $(ls -la $OUT/shell_x$scale.png 2>/dev/null | awk '{print $5" bytes"}')"
  done
  cp -r $OUT/appdata/DeckHand/deckhand.log $OUT/deckhand_app.log 2>/dev/null
  rm -rf $OUT/appdata   # holds a throw-away API token
  # the plain window path: start, let it draw for a few seconds, take an X screenshot of the Wine window
  (DECKHAND_LOG=debug wine64 $BUILD/deckhand.exe >$OUT/window.log 2>&1 &) ; sleep 12
  import -window root $OUT/window_xvfb.png 2>/dev/null || true
  pkill -f deckhand.exe; sleep 1
  wineserver -k 2>/dev/null
fi
echo "done"
