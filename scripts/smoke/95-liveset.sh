# Sourced by scripts/host-smoke.sh (not run on its own): defines liveset_check <dir>, which runs its session(s) in
# <dir>, prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: png_check,
# level_check, $here, $tag, $p, $exe, $r (renderer), $check, $headless.
#
# CHECK=liveset is the live set check ("a 30-minute live set without the
#   mouse"; tools/liveset-check.py describes the session and every check): the generated kit on scenes 1-2, one long
#   run whose script uses only panel controls and UI-120 keyboard keys (pads in every pad mode, step entry and hold +
#   turn locks, the LFO / AMP ENVELOPE / SEQUENCE / SET SLICE / SET SCALE / UTILITY / TOUCH STRIP SETTING / SCENE
#   MANAGER screens, ratchets, transpose, step shift, the strip in PITCH / REPEAT / USER1 / USER2 / HOLD, scene and
#   pattern switches, chains, pattern length, step copy / paste, tap tempo, recording passes, mutes and solos, pause /
#   stop, saves), with seeded randomness (LIVESET_SEED, default 1604; printed). Then tools/liveset-check.py verify: exit
#   code 0, no FATAL, no REALTIME VIOLATION (APP_RT_CHECKS aborts), xruns 0 in every AUDIO / LOAD line, LOAD p95
#   <= 725 000 cycles/block in every window (50 % of the block at 1 GHz; desktop ns, a relative gate), overruns
#   reported, every scripted screen's NAV line, every save ok, VmRSS growth bounded, no stuck notes after STOP, STOP
#   (MAIN silent over the last 3 s), every feature's evidence (the coverage list), and `cmd` only for checkpoints and the
#   NFR-040 gaps (each marked; `tools/liveset-check.py coverage` prints the audit).
# Length: LIVESET_MINUTES (minimum 2: the first ~100 s cover every feature once). Default 30 with CHECK=liveset, 2 when
#   it runs as part of CHECK=all (the per-merge smoke stays short). The run's timeout is the session + 2 minutes (the
#   dispatcher's launch() caps at 300 s, so this check starts the app itself, with launch()'s environment).
# Artefacts: log.txt, script.txt, manifest.json, verify.txt, kit.json; main.wav (MAIN, 16-bit stereo, ~10 MB a
#   minute) is deleted after a passing check unless LIVESET_KEEP_WAV=1.
liveset_check() {
  dir=$1
  minutes=${LIVESET_MINUTES:-}
  if [ -z "$minutes" ]; then
    if [ "$check" = liveset ]; then minutes=30; else minutes=2; fi
  fi
  seed=${LIVESET_SEED:-1604}
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  python3 "$here/tools/liveset-check.py" script --out "$dir/script.txt" --manifest "$dir/manifest.json" \
    --minutes "$minutes" --seed "$seed" || { why="$why; script generation failed"; return; }
  limit=$(python3 -c "import json; print(int(json.load(open('$dir/manifest.json'))['quit_ms'] / 1000) + 120)")
  vendor=${__EGL_VENDOR_LIBRARY_FILENAMES:-}
  mesa=/usr/share/glvnd/egl_vendor.d/50_mesa.json
  [ -n "$vendor" ] || { [ -f "$mesa" ] && vendor=$mesa; } || true
  echo "--- live set: $minutes min, seed $seed, timeout $limit s"
  set +e
  if [ "$headless" = 1 ]; then
    env -u WAYLAND_DISPLAY HOME="$dir/home" XDG_CONFIG_HOME="$dir/config" XDG_CACHE_HOME="$dir/cache" \
      LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=x11 ${vendor:+"__EGL_VENDOR_LIBRARY_FILENAMES=$vendor"} \
      "${p}_PLATFORM=linux" "${p}_DATA_DIR=$dir/data" "${p}_AUDIO_DEVICE=file:$dir/main.wav" "${p}_RENDER=$r" \
      "${p}_COMPARE=${COMPARE:-0}" "${p}_INPUT_SCRIPT=$dir/script.txt" "${p}_DEV_KIT=Samples/TestKit" \
      "${p}_DEV_KIT_SCENES=2" "${p}_LAMP_LOG=1" \
      timeout "$limit" xvfb-run -a -s "-screen 0 800x480x24" "$exe" > "$dir/log.txt" 2>&1
  else
    env HOME="$dir/home" XDG_CONFIG_HOME="$dir/config" XDG_CACHE_HOME="$dir/cache" \
      "${p}_PLATFORM=linux" "${p}_DATA_DIR=$dir/data" "${p}_AUDIO_DEVICE=file:$dir/main.wav" "${p}_RENDER=$r" \
      "${p}_COMPARE=${COMPARE:-0}" "${p}_INPUT_SCRIPT=$dir/script.txt" "${p}_DEV_KIT=Samples/TestKit" \
      "${p}_DEV_KIT_SCENES=2" "${p}_LAMP_LOG=1" timeout "$limit" "$exe" > "$dir/log.txt" 2>&1
  fi
  code=$?
  set -e
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|PROBE|AUDIO|RUN|LOAD|KIT|SAMPLE|FATAL|SCRIPT|SAVED|STOP)( |$)" \
    | sed "s/^.*$tag //" | awk 'NR <= 12 || /FATAL|SAVED|STOP|SAMPLE|PROBE/ { print; next } /^LOAD/ { last = $0 }
      END { if (last != "") print "(last) " last }' || true
  grep -F "REALTIME VIOLATION" "$dir/log.txt" || true
  echo "--- verify"
  python3 "$here/tools/liveset-check.py" verify --log "$dir/log.txt" --manifest "$dir/manifest.json" \
    --script "$dir/script.txt" --wav "$dir/main.wav" --exit-code "$code" > "$dir/verify.txt" 2>&1 \
    || why="$why; live-set checks failed"
  cat "$dir/verify.txt"
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
  if [ -z "$why" ] && [ "${LIVESET_KEEP_WAV:-0}" != 1 ]; then rm -f "$dir/main.wav"; fi
}
