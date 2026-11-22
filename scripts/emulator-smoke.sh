#!/bin/sh
# End-to-end smoke test in the emulator (local only: needs your own imported firmware; never runs in CI). It is the
# pad/kit check (16 pads play a kit with clean levels):
#   1. generate the 16-sample demo kit (tools/gen-test-kit.py: synthetic tones, noise bursts, chords, chirps — never
#      device data) into the mod's overlay as the app's data volume folder /root/data/<name>/Samples/TestKit
#   2. install the device binary (scripts/install.sh) with the renderer comparison on and dev_kit=Samples/TestKit, so
#      the app loads the kit onto tracks 1-16 with the real LOAD TO = FOLDER commands at start-up
#   3. launch the research build with file audio, unattended (scripts/run.sh --bench)
#   4. wait for the app's "KIT ready" line, hit pads 1-16 once each, 500 ms apart, turn knob 1 down 20 detents and
#      back, snapshot the LCD
#   5. stop the emulator by its exact PID, check the recorded master WAV on MAIN (device channels 12/14 = WAV channels
#      0 and 2) from 1 s before the first hit: 16 onsets matching the kit in order, peak <= -0.3 dBFS, no click above
#      -40 dBFS outside the samples (tools/analyze-wav.py --kit); and the app log: 0 xruns; LOAD p95 against the kit's
#      share (181 k cycles per block on the A9) and engine overruns are reported, not gated (qemu timing is relative)
#   6. print the frame-time lines (FRAME ...) the app logged
# KIT=0 runs the tone check instead (no kit: the fallback 440 Hz test tone on track 1, pads 5 and 1).
# Output: the session folder (console.txt, home.png, the WAVs). Exit 0 = all checks passed.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
eval "$(python3 "$here/tools/identity.py" --shell)"
emu=${SP16_EMULATOR:-$HOME/src/sp16-emulator}
build=${SP16_BUILD:-sp16-dev}
tag="[sp16-mod] $PROJECT_ENV_PREFIX"
kit=${KIT:-1}
mod=${APP_MOD:-$emu/private/$PROJECT_NAME}
kitdir=$mod/files/root/data/$PROJECT_NAME/Samples/TestKit
manifest=$mod/test-kit.json

if pgrep -f "build/$build/.*MacOS/SP16Emulator" >/dev/null; then
  echo "an emulator session of $build is already running; stop it first (kill <its pid>)" >&2; exit 2
fi
if [ "$kit" = 1 ]; then
  rm -rf "$kitdir"
  python3 "$here/tools/gen-test-kit.py" "$kitdir" --json "$manifest"
  "$here/scripts/install.sh" "render=${RENDER:-auto}" compare=1 "compare_frames=${FRAMES:-300}" dev_kit=Samples/TestKit
else
  rm -rf "$kitdir"
  "$here/scripts/install.sh" "render=${RENDER:-auto}" compare=1 "compare_frames=${FRAMES:-300}" test_tone=1
fi
out=$("$here/scripts/run.sh" --bench)
echo "$out"
session=$(echo "$out" | sed -n 's/^session: //p')
pid=$(echo "$out" | sed -n 's/^emulator pid: //p')
case "$pid" in ''|unknown|*[!0-9]*) echo "could not find the emulator PID" >&2; exit 2;; esac
stop() { kill "$pid" 2>/dev/null || true; for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || return 0; sleep 1; done; }
trap stop EXIT

echo "waiting for the app (RUN line in $session/console.txt)..."
for _ in $(seq 1 240); do grep -q "$tag RUN" "$session/console.txt" 2>/dev/null && break; sleep 1; done
grep -q "$tag RUN" "$session/console.txt" || { echo "app did not reach RUN"; grep "$tag" "$session/console.txt" || true; exit 1; }
cd "$emu"
if [ "$kit" = 1 ]; then
  echo "waiting for the kit (KIT ready)..."
  for _ in $(seq 1 120); do grep -q "$tag KIT \(ready\|FAILED\)" "$session/console.txt" 2>/dev/null && break; sleep 1; done
  grep -q "$tag KIT ready" "$session/console.txt" || { echo "the kit did not load"; grep "$tag" "$session/console.txt" || true; exit 1; }
  sleep 2
  first=$(python3 -c 'import time; print(time.time())') # the analysis window starts 1 s before the first hit
  # `./sp16 ctl` takes 0.3-0.8 s per call, so its underlying sessionctl.py drives the session's runtime.json. The hits
  # are 500 ms apart, not the desktop's 250 ms: the emulator delivers panel input with up to ~400 ms of jitter, and two
  # 100 ms samples that overlap fail the check. The spacing the WAV shows is reported by the analysis.
  ctl="./sp16 ctl --build $build"
  [ -f "$session/runtime.json" ] && ctl="python3 emulator/sessionctl.py --runtime $session/runtime.json"
  for n in $(seq 1 16); do
    t0=$(python3 -c 'import time; print(time.time())')
    $ctl press "pad$n" 0.05 110 >/dev/null
    python3 -c "import time; time.sleep(max(0.0, $t0 + 0.5 - time.time()))"
  done
  sleep 1
else
  sleep 3
  ./sp16 ctl --build "$build" press pad5 0.3 110 >/dev/null
  sleep 0.4
  ./sp16 ctl --build "$build" press pad1 0.3 100 >/dev/null
fi
./sp16 ctl --build "$build" turn knob1 -20 >/dev/null
sleep 1.5
./sp16 ctl --build "$build" snapshot home-turned.png >/dev/null
./sp16 ctl --build "$build" turn knob1 20 >/dev/null
sleep 4
./sp16 ctl --build "$build" snapshot home.png >/dev/null
sleep 8 # let the app print one 10 s "FRAME run" report
stop
stopped=$(python3 -c 'import time; print(time.time())')
trap - EXIT
echo "emulator $pid stopped"

echo "--- app log"
grep "$tag" "$session/console.txt" | sed "s/^.*$tag //"
wav=$(ls "$session"/*ak4458*.wav 2>/dev/null | head -1 || true)
[ -n "$wav" ] || { echo "no master WAV in $session (is $build an --audio file build?)" >&2; exit 1; }
echo "--- audio"
# The emulator's WAV keeps master channels 12..15 (audio-jack-map.md): WAV ch0 = MAIN L (12), ch2 = MAIN R (14).
rc=0
if [ "$kit" = 1 ]; then
  # The WAV ends when the emulator stops: the hits started (stopped - first) s before its end.
  from=$(python3 -c "import wave; w = wave.open('$wav'); print(max(0.0, w.getnframes() / w.getframerate() - ($stopped - $first) - 1.0))")
  python3 "$here/tools/analyze-wav.py" "$wav" --channels 0,2 --kit "$manifest" --kit-dir "$kitdir" --from-s "$from" || rc=1
else
  python3 "$here/tools/analyze-wav.py" "$wav" --channels 0,2 --expect-hz 440 --seconds 3 || rc=1
fi
echo "--- engine"
log=$(grep "$tag" "$session/console.txt" | sed "s/^.*$tag //")
echo "$log" | grep -E '^(AUDIO alsa|LOAD|FRAME run)' | tail -3
last=$(echo "$log" | grep '^LOAD ' | tail -1)
[ -n "$last" ] || { echo "no LOAD line"; rc=1; }
xruns=$(echo "$last" | sed -n 's/.* xruns=\([0-9]*\).*/\1/p')
overruns=$(echo "$last" | sed -n 's/.* overruns=\([0-9]*\).*/\1/p')
p95=$(echo "$last" | sed -n 's/.* p95=\([0-9]*\).*/\1/p')
echo "xruns=${xruns:-?} overruns=${overruns:-?} LOAD p95=${p95:-?} cycles/block (kit share 181000 on the A9; qemu is relative only)"
[ "${xruns:-1}" = 0 ] || { echo "xruns: FAILED"; rc=1; }
[ "${overruns:-0}" = 0 ] || echo "note: $overruns engine overruns (blocks over their period: qemu timing, not gated)"
[ "${p95:-0}" -le 181000 ] 2>/dev/null || echo "note: LOAD p95 above the A9 share (qemu timing, not gated)"
if [ "$kit" = 1 ]; then
  echo "$log" | grep -q '^SAMPLE ' && { echo "sample errors:"; echo "$log" | grep '^SAMPLE '; rc=1; }
fi
echo "--- screenshot: $session/home.png"
exit $rc
