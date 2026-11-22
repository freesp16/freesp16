# Sourced by scripts/host-smoke.sh (not run on its own): defines routing_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch,
# png_check, level_check, $here, $tag, $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=routing (in CHECK=all too) is the output routing check (tools/routing-check.py describes the session and every
#   check): one run with the file device writing every logical port (audio_file_ports=all, 12 WAV channels, MAIN still
#   0/1) and a 1 kHz LINE IN stand-in (audio_input=file:…); `cmd param` routes the 440 Hz test tone of track 1 to
#   OUT3-4, makes track 2 THRU, switches to 8CH and the HP CUE to OUT3-4; tools/routing-check.py verify: the tones on
#   MAIN / OUT3-4 / PHONES per phase (M + 6CH keeps the track on MAIN, 8CH only on its pair), silent unused ports, xruns 0.
# The output routing check of one renderer (see the header): prints its lines, sets $why.
routing_check() {
  dir=$1
  python3 "$here/tools/routing-check.py" script --out "$dir/script.txt" --input-wav "$dir/in.wav"
  launch "$dir" "$dir/script.txt" "$dir/log.txt" "${p}_AUDIO_FILE_PORTS=all" "${p}_AUDIO_INPUT=file:$dir/in.wav"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|AUDIO|RUN|LOAD|FATAL|PARAM|STOP|CMD)( |$)" \
    | sed "s/^.*$tag //" || true
  echo "--- verify"
  if [ -f "$dir/main.wav" ]; then
    python3 "$here/tools/routing-check.py" verify --log "$dir/log.txt" --wav "$dir/main.wav" > "$dir/verify.txt" 2>&1 \
      || why="$why; routing checks failed"
    cat "$dir/verify.txt"
  else
    why="$why; no WAV"
  fi
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
}
