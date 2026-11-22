# Sourced by scripts/host-smoke.sh (not run on its own): defines perf_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch,
# png_check, level_check, $here, $tag, $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=perf (in CHECK=all too) is the ratchet check (tools/perf-check.py describes the session and every check):
#   one run with the generated kit on scene 1: step keys enter four FULL steps on track 1, `cmd lock` gives them short
#   bursts (HOLD 4 ms) and RETRIGGER CNT / SPD locks, `cmd render` renders offline; then tools/perf-check.py verify:
#   the cells, one onset on MAIN per step and per ratchet repeat at t0 + round(k * step / divisor) (repeats meeting the
#   track's next step cancelled), within 3 ms, none extra, xruns 0. Transpose and step shift are unit-tested only.
# The ratchet check of one renderer (see the header): prints its lines, sets $why.
perf_check() {
  dir=$1
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  python3 "$here/tools/perf-check.py" script --out "$dir/script.txt" --out-dir "$dir"
  launch "$dir" "$dir/script.txt" "$dir/log.txt" "${p}_DEV_KIT=Samples/TestKit"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|KIT|SAMPLE|FATAL|M2 PATTERN|RENDER|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  echo "--- verify"
  python3 "$here/tools/perf-check.py" verify --log "$dir/log.txt" --render "$dir/perf.wav" > "$dir/verify.txt" 2>&1 \
    || why="$why; perf checks failed"
  cat "$dir/verify.txt"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
