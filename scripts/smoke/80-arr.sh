# Sourced by scripts/host-smoke.sh (not run on its own): defines arr_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch,
# png_check, level_check, $here, $tag, $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=arr (in CHECK=all too) is the song mode check (tools/arr-check.py describes the session and every
#   check): one run, transport stopped: `cmd` row commands build a 5-row arrangement (pattern lengths 16 / 8 / 64 / 16 /
#   8, repeats, fixed and "--" BPMs) and undo duplicate / move / delete / a knob turn one step each, `cmd arr-play 1`,
#   `cmd render ... song` renders it offline in song mode; then tools/arr-check.py verify: the ARR lines, the render's
#   switch frames = the row starts modelled block by block, the song position (SPP) at every row start = the start
#   table, the stop in the end block, xruns 0.
# The song mode check of one renderer (see the header): prints its lines, sets $why.
arr_check() {
  dir=$1
  python3 "$here/tools/arr-check.py" script --out "$dir/script.txt" --out-dir "$dir"
  launch "$dir" "$dir/script.txt" "$dir/log.txt"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|FATAL|ARR|RENDER|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  echo "--- verify"
  python3 "$here/tools/arr-check.py" verify --log "$dir/log.txt" > "$dir/verify.txt" 2>&1 \
    || why="$why; arr checks failed"
  cat "$dir/verify.txt"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
