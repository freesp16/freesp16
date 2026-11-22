# Sourced by scripts/host-smoke.sh (not run on its own): defines rib_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch,
# png_check, level_check, $here, $tag, $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=rib (in CHECK=all too) is the touch strip check (tools/rib-check.py describes the session and every
#   check): one run with the generated kit and lamp_log=1, the transport stopped: MODE -> REPEAT, pad 1 held with
#   `strip 40` (1/8) for 2 s, then HOLD + pad 2 with `strip 80` (1/16) lifted after 0.5 s, the pad released 1 s later;
#   the MODE cycle back to PITCH and the TOUCH STRIP SETTING commands (`cmd ribbon-assign/-range`, `cmd ribbon`).
#   tools/rib-check.py verify: the RIBBON and LAMP lines (mode lamps, the zone's two LEDs, HOLD), the repeats on MAIN
#   250 / 125 ms apart (+/- 2 ms), the HOLD continuation and its end, xruns 0. PITCH / USER are not audible in this
#   check (they act through the LFO's ModMatrix; tests/unit/ribbon covers them).
# The touch strip check of one renderer (see the header): prints its lines, sets $why.
rib_check() {
  dir=$1
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  python3 "$here/tools/rib-check.py" script --out "$dir/script.txt"
  launch "$dir" "$dir/script.txt" "$dir/log.txt" "${p}_DEV_KIT=Samples/TestKit" "${p}_LAMP_LOG=1"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|KIT|SAMPLE|FATAL|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  last_audio=$(grep -F "$tag AUDIO alsa_periods=" "$dir/log.txt" | tail -1)
  case "$last_audio" in *" xruns=0 "*) ;; *) why="$why; AUDIO xruns != 0 or missing";; esac
  grep -qF "$tag KIT ready installed=16 " "$dir/log.txt" || why="$why; the kit did not load"
  echo "--- verify"
  if [ -f "$dir/main.wav" ]; then
    python3 "$here/tools/rib-check.py" verify --log "$dir/log.txt" --wav "$dir/main.wav" > "$dir/verify.txt" 2>&1 \
      || why="$why; rib checks failed"
    cat "$dir/verify.txt"
  else
    why="$why; no WAV"
  fi
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
