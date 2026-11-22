# Sourced by scripts/host-smoke.sh (not run on its own): defines import_check <dir>, which runs its session in <dir>,
# prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch, $here, $tag,
# $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=import (in CHECK=all too) is the project import check (tools/prj-dump/import-smoke.py describes the session and
#   every check): a reference project is generated from tests/unit/interop/synthetic/basic.json into the run folder
#   (never a device file), the app imports it with `cmd import-sp16` (the sp16-prj codec through ProjectStore::openAs,
#   samples in place), renders it offline (`cmd render … 3`) and logs its state; then verify: the IMPORT report lines,
#   the pattern, the render's onsets at the FULL step times (+/- 5 ms), the volume lock audible, the source tree
#   unchanged (FMT-045), xruns 0.
import_check() {
  dir=$1
  python3 "$here/tools/prj-dump/import-smoke.py" script --out-dir "$dir" > "$dir/gen.txt" 2>&1 || why="$why; generation failed"
  launch "$dir" "$dir/script.txt" "$dir/log.txt"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|FATAL|IMPORT|PROJECT|M2 PATTERN|RENDER|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  echo "--- verify"
  python3 "$here/tools/prj-dump/import-smoke.py" verify --log "$dir/log.txt" --out-dir "$dir" > "$dir/verify.txt" 2>&1 \
    || why="$why; import checks failed"
  cat "$dir/verify.txt"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
