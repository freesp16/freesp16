# Sourced by scripts/host-smoke.sh (not run on its own): defines slice_check <dir>, which runs its session in <dir>,
# prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch, $here, $tag,
# $p, $code (launch's exit code).
#
# CHECK=slice (in CHECK=all too) is the slice check (tools/slice-check.py describes the session and every
# check): one run with the generated kit on scene 1: track 1 cut into 8 slices with one moved point (`cmd slice`), four
# SLICE pad hits each followed by a step key (step entry writes the hit's slice as the slice-number lock), `cmd render`
# offline; then tools/slice-check.py verify: the slice points, the pads' slices, the locked cells, one onset on MAIN
# per step at its frame, each step's audio = the sample's slice frames from that exact frame (moved point included),
# silence after each slice end, xruns 0.
slice_check() {
  dir=$1
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  python3 "$here/tools/slice-check.py" script --out "$dir/script.txt" --out-dir "$dir"
  launch "$dir" "$dir/script.txt" "$dir/log.txt" "${p}_DEV_KIT=Samples/TestKit"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|KIT|FATAL|SLICE|TRANSPORT|INPUT pad[0-9]+ slice=[0-9-]+|M2 PATTERN|RENDER|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  echo "--- verify"
  python3 "$here/tools/slice-check.py" verify --log "$dir/log.txt" --render "$dir/slice.wav" \
    --kit-dir "$dir/data/Samples/TestKit" --kit-json "$dir/kit.json" > "$dir/verify.txt" 2>&1 || why="$why; slice checks failed"
  cat "$dir/verify.txt"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
