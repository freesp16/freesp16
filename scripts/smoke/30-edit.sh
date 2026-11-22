# Sourced by scripts/host-smoke.sh (not run on its own): defines edit_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch,
# png_check, level_check, $here, $tag, $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=edit (in CHECK=all too) is the pattern edit check (tools/edit-check.py describes the session and every check):
#   one run with the generated kit on scene 1: step keys enter pattern 1, `cmd edit-copy/edit-paste` copies it to
#   pattern 2 (the SceneEditor, prepared on a worker), ext.copy / ext.paste + step keys copy two steps into pattern 4,
#   the PATTERN keys build the chain 1->2->2->4 (hold the current key, press the others), `cmd render ... chain` renders
#   it offline; then tools/edit-check.py verify: the cells, EDIT lines, the render's switch frames = the pass ends, one
#   onset per FULL step of every chained pass (the pasted patterns audible through the sequencer), xruns 0.
#   The script's actions are ordered by the UI thread (the render waits for pending pastes; quit comes after it).
# The pattern edit check of one renderer (see the header): prints its lines, sets $why.
edit_check() {
  dir=$1
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  python3 "$here/tools/edit-check.py" script --out "$dir/script.txt" --out-dir "$dir"
  launch "$dir" "$dir/script.txt" "$dir/log.txt" "${p}_DEV_KIT=Samples/TestKit"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|KIT|SAMPLE|FATAL|EDIT|M2 PATTERN|RENDER|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  echo "--- verify"
  python3 "$here/tools/edit-check.py" verify --log "$dir/log.txt" --render "$dir/edit.wav" > "$dir/verify.txt" 2>&1 \
    || why="$why; edit checks failed"
  cat "$dir/verify.txt"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
