# Sourced by scripts/host-smoke.sh (not run on its own): defines m2_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch,
# png_check, level_check, $here, $tag, $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=m2 (in CHECK=all too) is the sequencer check (build, save, resume and render a beat, on the
# desktop; tools/m2-check.py describes both sessions and every check):
#   1. the generated kit on tracks 1-16 of scenes 1-4 (dev_kit_scenes=4); run 1's script builds a 4-scene beat through
#      the panel keys (SCENE mode, pads, step keys) and `cmd` lines (TRIGGER TYPE, one pitch lock per pattern, scene
#      BPMs), records one pass, queues scenes 1->4 while playing, saves as beat4, renders offline (render1.wav) and plays
#      on; once "RENDER file=" is logged the app is killed with SIGKILL (its pid from the START line)
#   2. run 2 (same data dir, no kit) resumes, logs the state and renders render2.wav
#   3. tools/m2-check.py verify: the beat was built, the resume reopened beat4 with the same content and selection,
#      both renders hash equal, the render's scene switches on the modelled pass-end frames, its onsets on MAIN at the
#      FULL step times (+/- 5 ms, none extra), LOAD p95 <= 202000 cycles/block (the sequencer's share), xruns 0.
#   Not automated: the live WAV's onsets (the script clock vs the audio clock is not sample-exact; the offline
#   render carries the timing checks), the lamps and the screens (left to a manual check).
# The sequencer check of one renderer (see the header): prints its lines, sets $why.
m2_check() {
  dir=$1
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  python3 "$here/tools/m2-check.py" script --run1 "$dir/run1.txt" --run2 "$dir/run2.txt" --out-dir "$dir"
  BACKGROUND=1 launch "$dir" "$dir/run1.txt" "$dir/log1.txt" "${p}_DEV_KIT=Samples/TestKit" "${p}_DEV_KIT_SCENES=4"
  bg=$!
  waited=0
  # The kill waits for both the render and the save's own result line (the save runs on a worker: on a loaded machine
  # it can still be pending when the render is logged).
  until { grep -qF "$tag RENDER file=" "$dir/log1.txt" && grep -qF "$tag SAVED beat4 " "$dir/log1.txt"; } 2>/dev/null \
        || [ $waited -ge 240 ] || ! kill -0 $bg 2>/dev/null; do
    sleep 1; waited=$((waited + 1))
  done
  sleep 2 # playing again when the kill comes
  pid=$(grep -F "$tag START " "$dir/log1.txt" | sed -n 's/.* pid=\([0-9]*\).*/\1/p' | head -1)
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid"
    echo "--- run 1: killed pid $pid (SIGKILL) after ${waited} s"
  else
    why="$why; run 1 ended before the kill (no pid or no RENDER)"
  fi
  wait $bg 2>/dev/null || true
  mv "$dir/main.wav" "$dir/main1.wav" 2>/dev/null || true
  launch "$dir" "$dir/run2.txt" "$dir/log2.txt"
  [ "$code" = 0 ] || why="$why; run 2 exit code $code"
  for f in log1 log2; do
    echo "--- $f"
    grep -F "$tag " "$dir/$f.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|KIT|SAMPLE|FATAL|PROJECT|SAVED|M2 STATE|M2 SWITCH|RENDER|STOP|CMD unknown)( |$)" \
      | sed "s/^.*$tag //" || true
  done
  echo "--- verify"
  python3 "$here/tools/m2-check.py" verify --log1 "$dir/log1.txt" --log2 "$dir/log2.txt" \
    --render1 "$dir/render1.wav" --render2 "$dir/render2.wav" --load-max 202000 > "$dir/verify.txt" 2>&1 \
    || why="$why; m2 checks failed"
  cat "$dir/verify.txt"
  grep -qF "$tag STOP" "$dir/log2.txt" || why="$why; run 2: no STOP"
  first="$dir/log1.txt" # run 1 carries the RUN/PROBE lines
}
