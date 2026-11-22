# Sourced by scripts/host-smoke.sh (not run on its own): defines hwready_check <dir>, which runs its sessions in <dir>,
# prints its lines and appends failures to $why. Helpers and variables come from host-smoke.sh: launch, $here, $tag,
# $p, $exe, $r (renderer), $code (launch's exit code).
#
# CHECK=hwready (in CHECK=all too) is the hardware-readiness check: the hardware-test bundle's scripts on the desktop
# (tools/hw-test-bundle.py; docs/hardware-test.md), so what runs on the unit is known to work here first.
#   1. selftest: the generated kit on scenes 1-2 and the bundle's self-test script (pads, pattern, scene switch,
#      routing, analog filter, resample take, render, save). Settings come from all three sources: the defaults, the
#      platform's settings file (written into the run's XDG config: `Lamp_Log = 1`, `diag_interval=5` — a key the
#      defaults table does not have must still reach the platform: LAMP lines appear) and <PREFIX>_* variables
#      (including <PREFIX>_HWREADY__PROBE=1 → hwready.probe=1). Verify: tools/hw-test-bundle.py verify --kind selftest
#      --desktop (SETTINGS, the DIAG block … DIAG END, DIAG STOP, every MARK, PARAM posted, M2 SWITCH scene=2,
#      SAMPLING TAKE ok, RENDER JOB done, SAVED, xruns 0, no FATAL / CRASH, STOP) plus the settings lines above.
#   2. import: a synthetic reference disk (tools/prj-dump/import-smoke.py's generator, never device data) as the
#      internal disk (internal_disk=<dir>), the bundle's import script (`import-sp16 internal:` list, `internal:1`,
#      play, render, save); verify --kind import --desktop, and the disk unchanged (no file added or modified).
#   3. crash: the bundle's crash script (`cmd diag-crash segv`): the CRASH header, backtrace, maps and end lines in the
#      log after DIAG END, and the process died of SIGSEGV (not exit 0).
hwready_check() {
  dir=$1
  python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" > "$dir/gen-test-kit.txt"
  for k in selftest import crash; do
    python3 "$here/tools/hw-test-bundle.py" script --kind "$k" --out "$dir/$k.txt" --data-dir "$dir/data"
  done
  mkdir -p "$dir/config/$PROJECT_NAME"
  printf '# hwready smoke: keys outside the defaults table, mixed case\nLamp_Log = 1\ndiag_interval=5\n' \
    > "$dir/config/$PROJECT_NAME/$PROJECT_NAME.conf"
  # 1. selftest
  launch "$dir" "$dir/selftest.txt" "$dir/log.txt" "${p}_DEV_KIT=Samples/TestKit" "${p}_DEV_KIT_SCENES=2" \
    "${p}_HWREADY__PROBE=1"
  [ "$code" = 0 ] || why="$why; selftest exit code $code"
  grep -F "$tag " "$dir/log.txt" | grep -E " (SETTINGS|DIAG (platform|cpu|libc|display|audio device|END|STOP)|MARK|FATAL|CRASH|SAMPLING TAKE|RENDER JOB done|SAVED|STOP)( |$)" \
    | sed "s/^.*$tag //" | cut -c1-220 || true
  echo "--- verify selftest"
  python3 "$here/tools/hw-test-bundle.py" verify --kind selftest --desktop --log "$dir/log.txt" > "$dir/verify.txt" 2>&1 \
    || why="$why; selftest checks failed"
  cat "$dir/verify.txt"
  settings=$(grep -F "$tag SETTINGS " "$dir/log.txt" | head -1)
  for want in "lamp_log=1" "diag_interval=5" "hwready.probe=1" "dev_kit=Samples/TestKit" "(read) file_keys=2"; do
    case "$settings" in *"$want"*) echo "OK    SETTINGS has $want" ;;
      *) echo "FAIL  SETTINGS has $want"; why="$why; SETTINGS lacks $want" ;; esac
  done
  if grep -qF "$tag LAMP " "$dir/log.txt"; then echo "OK    LAMP lines (lamp_log from the settings file reached the platform)"
  else echo "FAIL  no LAMP line"; why="$why; lamp_log from the file did not reach the platform"; fi
  # 2. import from a synthetic internal disk
  mkdir -p "$dir/imp"
  python3 "$here/tools/prj-dump/import-smoke.py" script --out-dir "$dir/imp" > "$dir/imp/gen.txt" 2>&1 \
    || why="$why; import disk generation failed"
  (cd "$dir/imp/disk" && find . -type f -exec sha256sum {} + | sort) > "$dir/imp/before.txt"
  launch "$dir" "$dir/import.txt" "$dir/log-import.txt" "${p}_INTERNAL_DISK=$dir/imp/disk"
  [ "$code" = 0 ] || why="$why; import exit code $code"
  grep -F "$tag " "$dir/log-import.txt" | grep -E " (DIAG storage internal|MARK|IMPORT|RENDER JOB done|SAVED|FATAL|STOP)( |$)" \
    | sed "s/^.*$tag //" | cut -c1-220 || true
  echo "--- verify import"
  python3 "$here/tools/hw-test-bundle.py" verify --kind import --desktop --log "$dir/log-import.txt" \
    > "$dir/verify-import.txt" 2>&1 || why="$why; import checks failed"
  cat "$dir/verify-import.txt"
  (cd "$dir/imp/disk" && find . -type f -exec sha256sum {} + | sort) > "$dir/imp/after.txt"
  if cmp -s "$dir/imp/before.txt" "$dir/imp/after.txt"; then echo "OK    the internal disk is unchanged"
  else echo "FAIL  the internal disk changed"; why="$why; internal disk written"; fi
  # 3. crash report
  launch "$dir" "$dir/crash.txt" "$dir/log-crash.txt"
  crash_code=$code
  echo "--- crash run: exit code $crash_code"
  grep -F "$tag CRASH" "$dir/log-crash.txt" | sed "s/^.*$tag //" | head -4 || true
  for want in "CRASH SIGSEGV (11)" "CRASH backtrace frames=" "CRASH map " "CRASH end"; do
    if grep -qF "$tag $want" "$dir/log-crash.txt"; then echo "OK    $want"
    else echo "FAIL  $want"; why="$why; crash report lacks '$want'"; fi
  done
  grep -qF "$tag DIAG END" "$dir/log-crash.txt" || why="$why; crash run: no DIAG END"
  [ "$crash_code" != 0 ] || why="$why; the crash run exited 0"
}
