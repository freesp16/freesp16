#!/bin/sh
# End-to-end smoke test on the Linux desktop (replaces scripts/emulator-smoke.sh as the per-merge check).
# Builds nothing: it runs build/${PRESET:-host-fast}/bin/<name> with the linux platform, once per renderer:
#   1. under `xvfb-run` (800x480x24, headless; HEADLESS=0 = the real desktop $DISPLAY), a temporary HOME / data /
#      cache / config, audio_device=file:<dir>/main.wav, render=<renderer>, and a generated input script: press pad 5
#      and pad 1, turn knob 1 -20 and back +20, tap the LCD, take a screenshot, quit after ~8 s
#   2. checks: RUN reached on that renderer, AUDIO ok, the WAV carries the 440 Hz test tone at -12 dBFS +/- 0.5 peak on
#      MAIN (WAV channels 0/1, tools/analyze-wav.py, last 4 s), xruns=0 in the last AUDIO and LOAD lines, the screenshot
#      is a PNG that decodes and is not one colour, exit code 0
#   3. prints the FRAME/LOAD/AUDIO lines and one PASS / FAIL / SKIP line per renderer. SKIP = the renderer is not
#      available here (e.g. gl-x11 without a usable Mesa EGL under Xvfb), with the app's reason.
# CHECK=kit (CHECK=all, the default, runs the tone check above and then this one, per renderer) is the pad/kit check
# (16 pads play a kit with clean levels):
#   1. tools/gen-test-kit.py writes the 16 synthetic one-shots into the run's data dir (Samples/TestKit = the app's
#      dev_kit=Samples/TestKit) and the input script: pads 1-16 once each, velocity 110, 250 ms apart from 2000 ms,
#      quit 1.5 s after the last hit. Script time 0 = the first poll; measured: the WAV's first frame is within a few
#      ms of it and the fallback test tone stops ~25 ms in (the kit is installed), so the analysis starts at 1.5 s
#   2. checks: RUN, AUDIO ok, "KIT ready installed=16", no SAMPLE error, tools/analyze-wav.py --kit on MAIN (ch 0/1)
#      from 1.5 s: 16 onsets, each its own pad's sample and the best match of all 16 (16 distinct hits in pad order),
#      250 +/- 40 ms apart, no sample above -0.3 dBFS, no step above -40 dBFS outside the samples; xruns=0 (AUDIO and
#      LOAD), LOAD p95 <= 181000 cycles/block (the kit's share of the A9 budget; 1 cycle = 1 ns, so desktop numbers
#      are relative), overruns reported (not gated), exit code 0, STOP
# More checks live in scripts/smoke/NN-<name>.sh, one file per check (each defines <name>_check and documents its
# session; a new check adds its own file instead of editing this one). CHECK=<name> runs one of them,
# CHECK=all (the default) runs tone, kit and then every scripts/smoke/ check in file-name order.
# Artefacts: out/host-smoke/<timestamp>/<renderer>[-<check>]/ (log.txt, main.wav, script.txt; tone: home.png; kit:
# data/, kit.json; m2: log1.txt, log2.txt, run1.txt, run2.txt, render1.wav, render2.wav, verify.txt).
# Exit: 0 = every requested renderer passed, 1 = one failed, 2 = none failed but one was skipped.
#   scripts/host-smoke.sh                  RENDERS="soft gl-x11" CHECK=all (default)
#   RENDERS=soft CHECK=kit PRESET=host-release HEADLESS=0 COMPARE=1 scripts/host-smoke.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
eval "$(python3 "$here/tools/identity.py" --shell)"
preset=${PRESET:-host-fast}
exe="$here/build/$preset/bin/$PROJECT_NAME"
renders=${RENDERS:-soft gl-x11}
headless=${HEADLESS:-1}
seconds=${SECONDS_RUN:-8}
check=${CHECK:-all}
extra=""
for f in "$here"/scripts/smoke/*.sh; do
  [ -f "$f" ] || continue
  m=$(basename "$f" .sh); extra="$extra ${m#*-}"
done
case " tone kit$extra all " in
  *" $check "*) ;;
  *) echo "CHECK=$check: one of tone kit$extra all" >&2; exit 2 ;;
esac
if [ "$check" = all ]; then modes="tone kit$extra"; else modes=$check; fi
kit_first_ms=2000 kit_spacing_ms=250 kit_from_s=1.5 # the kit analysis starts 500 ms before the first hit
load_p95_max=181000
tag="[sp16-mod] $PROJECT_ENV_PREFIX"
p=$PROJECT_ENV_PREFIX

[ -x "$exe" ] || { echo "no app at $exe: cmake --preset $preset && cmake --build --preset $preset" >&2; exit 2; }
if [ "$headless" = 1 ]; then
  command -v xvfb-run >/dev/null || { echo "xvfb-run not found (package xvfb)" >&2; exit 2; }
else
  [ -n "${DISPLAY:-}" ] || { echo "HEADLESS=0 needs DISPLAY" >&2; exit 2; }
fi
out="$here/out/host-smoke/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "artefacts: $out"

# PNG check, standard library only: decode (zlib + row filters), size, number of distinct colours.
png_check() {
  python3 - "$1" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
try:
    data = open(path, "rb").read()
except OSError as e:
    print(f"screenshot: {e}"); sys.exit(1)
if data[:8] != b"\x89PNG\r\n\x1a\n":
    print("screenshot: not a PNG"); sys.exit(1)
pos, idat, w = 8, b"", 0
while pos < len(data):
    n, kind = struct.unpack(">I4s", data[pos:pos + 8])
    body = data[pos + 8:pos + 8 + n]
    if kind == b"IHDR":
        w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
    elif kind == b"IDAT":
        idat += body
    pos += 12 + n
if not w or depth != 8 or ctype not in (2, 6):
    print(f"screenshot: unsupported PNG (depth {depth}, colour type {ctype})"); sys.exit(1)
bpp = 3 if ctype == 2 else 4
raw, stride = zlib.decompress(idat), w * bpp
prev, colours = bytearray(stride), set()
for y in range(h):
    f, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    for i in range(stride):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if f == 1: row[i] = (row[i] + a) & 255
        elif f == 2: row[i] = (row[i] + b) & 255
        elif f == 3: row[i] = (row[i] + (a + b) // 2) & 255
        elif f == 4:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    if y % 4 == 0:
        colours.update(bytes(row[i:i + 3]) for i in range(0, stride, bpp * 2))
    prev = row
print(f"screenshot: {w}x{h} PNG decodes, {len(colours)} distinct colours (sampled)")
sys.exit(0 if len(colours) > 1 else 1)
PY
}

# Peak of MAIN L/R from analyze-wav's output must be -12 dBFS +/- 0.5.
level_check() {
  python3 -c '
import re, sys
peaks = [float(m) for m in re.findall(r"peak\s+(-?[\d.]+) dBFS", sys.stdin.read())]
ok = len(peaks) == 2 and all(abs(p + 12.0) <= 0.5 for p in peaks)
print("  level check:", "OK" if ok else "FAILED", "(MAIN peaks", peaks, "expected -12 +/- 0.5 dBFS)")
sys.exit(0 if ok else 1)'
}

# launch <dir> <script> <log> [VAR=value ...]: the app with the linux platform, the run's HOME / data / file audio
# device and the script; in the foreground (its exit code in $code), or with BACKGROUND=1 in the background ($! = pid).
launch() {
  ldir=$1 lscript=$2 llog=$3
  shift 3
  vendor=${__EGL_VENDOR_LIBRARY_FILENAMES:-}
  mesa=/usr/share/glvnd/egl_vendor.d/50_mesa.json
  [ -n "$vendor" ] || { [ -f "$mesa" ] && vendor=$mesa; } || true
  if [ "$headless" = 1 ]; then
    set -- env -u WAYLAND_DISPLAY HOME="$ldir/home" XDG_CONFIG_HOME="$ldir/config" XDG_CACHE_HOME="$ldir/cache" \
      LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=x11 ${vendor:+"__EGL_VENDOR_LIBRARY_FILENAMES=$vendor"} \
      "${p}_PLATFORM=linux" "${p}_DATA_DIR=$ldir/data" "${p}_AUDIO_DEVICE=file:$ldir/main.wav" "${p}_RENDER=$r" \
      "${p}_COMPARE=${COMPARE:-0}" "${p}_TEST_TONE=1" "${p}_INPUT_SCRIPT=$lscript" "$@" \
      timeout 300 xvfb-run -a -s "-screen 0 800x480x24" "$exe"
  else
    set -- env HOME="$ldir/home" XDG_CONFIG_HOME="$ldir/config" XDG_CACHE_HOME="$ldir/cache" \
      "${p}_PLATFORM=linux" "${p}_DATA_DIR=$ldir/data" "${p}_AUDIO_DEVICE=file:$ldir/main.wav" "${p}_RENDER=$r" \
      "${p}_COMPARE=${COMPARE:-0}" "${p}_TEST_TONE=1" "${p}_INPUT_SCRIPT=$lscript" "$@" timeout 300 "$exe"
  fi
  if [ "${BACKGROUND:-0}" = 1 ]; then
    "$@" > "$llog" 2>&1 &
  else
    set +e
    "$@" > "$llog" 2>&1
    code=$?
    set -e
  fi
}

# The scripts/smoke/ checks (functions only; see the header).
for f in "$here"/scripts/smoke/*.sh; do
  [ -f "$f" ] && . "$f"
done

summary=""
failed=0
skipped=0
for r in $renders; do
for mode in $modes; do
  name=$r
  [ "$mode" = tone ] || name=$r-$mode
  dir="$out/$name"
  mkdir -p "$dir/home" "$dir/cache" "$dir/config" "$dir/data"
  if [ "$mode" != tone ] && [ "$mode" != kit ]; then
    echo "=== $name"
    why=""
    first="$dir/log.txt"
    "${mode}_check" "$dir"
    if grep -qF "$tag PROBE $r" "$first" && ! grep -qF "$tag RUN backend=$r " "$first"; then
      line="SKIP $name: unavailable here: $(grep -F "$tag PROBE $r" "$first" | tail -1 | sed "s/^.*$tag PROBE $r[: ]*//")"
      skipped=$((skipped + 1))
    elif [ -z "$why" ]; then
      line="PASS $name"
    else
      line="FAIL $name:${why#;}"
      failed=$((failed + 1))
    fi
    echo "$line"
    summary="$summary$line
"
    continue
  fi
  kitenv=""
  if [ "$mode" = kit ]; then
    python3 "$here/tools/gen-test-kit.py" --data-dir "$dir/data" --json "$dir/kit.json" --input-script "$dir/script.txt" \
      --first-ms $kit_first_ms --spacing-ms $kit_spacing_ms > "$dir/gen-test-kit.txt"
    kitenv="${p}_DEV_KIT=Samples/TestKit"
  else
  cat > "$dir/script.txt" <<EOF
# generated by scripts/host-smoke.sh (time 0 = the app's first frame, ~ RUN)
1000 press pad5 110
1300 release pad5
1500 press pad1 100
1800 release pad1
2000 turn knob1 -20
3000 turn knob1 20
3500 tap 400 240
6000 screenshot $dir/home.png
$((seconds * 1000)) quit
EOF
  fi
  echo "=== $name"
  set +e
  # Headless: only Mesa (llvmpipe) can draw into Xvfb; keep libglvnd off the desktop's vendor and Wayland session.
  if [ "$headless" = 1 ]; then
    vendor=${__EGL_VENDOR_LIBRARY_FILENAMES:-}
    mesa=/usr/share/glvnd/egl_vendor.d/50_mesa.json
    [ -n "$vendor" ] || { [ -f "$mesa" ] && vendor=$mesa; } || true
    env -u WAYLAND_DISPLAY HOME="$dir/home" XDG_CONFIG_HOME="$dir/config" XDG_CACHE_HOME="$dir/cache" \
      LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=x11 ${vendor:+"__EGL_VENDOR_LIBRARY_FILENAMES=$vendor"} \
      "${p}_PLATFORM=linux" "${p}_DATA_DIR=$dir/data" "${p}_AUDIO_DEVICE=file:$dir/main.wav" "${p}_RENDER=$r" \
      "${p}_COMPARE=${COMPARE:-0}" "${p}_TEST_TONE=1" "${p}_INPUT_SCRIPT=$dir/script.txt" ${kitenv:+"$kitenv"} \
      timeout $((seconds + 60)) xvfb-run -a -s "-screen 0 800x480x24" "$exe" > "$dir/log.txt" 2>&1
  else
    env HOME="$dir/home" XDG_CONFIG_HOME="$dir/config" XDG_CACHE_HOME="$dir/cache" \
      "${p}_PLATFORM=linux" "${p}_DATA_DIR=$dir/data" "${p}_AUDIO_DEVICE=file:$dir/main.wav" "${p}_RENDER=$r" \
      "${p}_COMPARE=${COMPARE:-0}" "${p}_TEST_TONE=1" "${p}_INPUT_SCRIPT=$dir/script.txt" ${kitenv:+"$kitenv"} \
      timeout $((seconds + 60)) "$exe" > "$dir/log.txt" 2>&1
  fi
  code=$?
  set -e
  grep -F "$tag " "$dir/log.txt" | grep -E " (PLATFORM|PROBE|AUDIO|RUN|FRAME|LOAD|SCREENSHOT|SCRIPT|KIT|SAMPLE|FATAL|STOP)( |$)" \
    | sed "s/^.*$tag //" || true
  why=""
  if ! grep -qF "$tag RUN backend=$r " "$dir/log.txt"; then
    reason=$(grep -F "$tag PROBE $r" "$dir/log.txt" | tail -1 | sed "s/^.*$tag PROBE $r[: ]*//")
    if [ -n "$reason" ]; then
      line="SKIP $name: unavailable here: $reason"
      skipped=$((skipped + 1))
      echo "$line"
      summary="$summary$line
"
      continue
    fi
    why="$why; no RUN backend=$r"
  fi
  grep -qF "$tag AUDIO ok file:" "$dir/log.txt" || why="$why; no AUDIO ok (file device)"
  last_audio=$(grep -F "$tag AUDIO alsa_periods=" "$dir/log.txt" | tail -1)
  last_load=$(grep -F "$tag LOAD " "$dir/log.txt" | tail -1)
  case "$last_audio" in *" xruns=0 "*) ;; *) why="$why; AUDIO xruns != 0 or missing";; esac
  case "$last_load" in *" xruns=0 "*) ;; *) why="$why; LOAD xruns != 0 or missing";; esac
  echo "--- audio"
  if [ "$mode" = kit ]; then
    grep -qF "$tag KIT ready installed=16 " "$dir/log.txt" || why="$why; the kit did not load (no KIT ready installed=16)"
    if grep -qF "$tag SAMPLE " "$dir/log.txt"; then why="$why; SAMPLE errors"; fi
    if [ -f "$dir/main.wav" ]; then
      wav_out=$(python3 "$here/tools/analyze-wav.py" "$dir/main.wav" --channels 0,1 --kit "$dir/kit.json" \
        --kit-dir "$dir/data/Samples/TestKit" --from-s $kit_from_s --spacing-ms $kit_spacing_ms 2>&1) \
        || why="$why; kit check failed on MAIN"
      echo "$wav_out"
      # The script clock vs the WAV's first frame: where the first hit landed (the gate opens ~2 ms into a hit).
      first=$(echo "$wav_out" | sed -n 's/^ *onset  1 at *\([0-9.]*\) s.*/\1/p')
      [ -z "$first" ] || python3 -c "print('  script clock vs WAV: first hit at %.3f s, scripted %.3f s (%+.0f ms)' % \
        ($first, $kit_first_ms / 1000.0, 1000 * ($first - $kit_first_ms / 1000.0)))"
    else
      why="$why; no WAV"
    fi
    p95=$(echo "$last_load" | sed -n 's/.* p95=\([0-9]*\).*/\1/p')
    overruns=$(echo "$last_load" | sed -n 's/.* overruns=\([0-9]*\).*/\1/p')
    echo "--- engine: LOAD p95=${p95:-?} cycles/block (limit $load_p95_max), overruns=${overruns:-?} (reported, not gated)"
    if [ -z "$p95" ] || [ "$p95" -gt $load_p95_max ]; then why="$why; LOAD p95 ${p95:-missing} above $load_p95_max"; fi
  else
    if [ -f "$dir/main.wav" ]; then
      wav_out=$(python3 "$here/tools/analyze-wav.py" "$dir/main.wav" --channels 0,1 --expect-hz 440 --seconds 4 2>&1) \
        || why="$why; 440 Hz tone missing on MAIN"
      echo "$wav_out"
      echo "$wav_out" | level_check || why="$why; MAIN level not -12 dBFS"
    else
      why="$why; no WAV"
    fi
    echo "--- screenshot: $dir/home.png"
    grep -qF "$tag SCREENSHOT $dir/home.png " "$dir/log.txt" || why="$why; no SCREENSHOT line"
    png_check "$dir/home.png" || why="$why; screenshot blank or unreadable"
  fi
  [ "$code" = 0 ] || why="$why; exit code $code"
  grep -qF "$tag STOP" "$dir/log.txt" || why="$why; no STOP"
  if [ -z "$why" ]; then
    line="PASS $name"
  else
    line="FAIL $name:${why#;}"
    failed=$((failed + 1))
  fi
  echo "$line"
  summary="$summary$line
"
done
done
echo "=== summary ($out)"
printf "%s" "$summary"
[ "$failed" = 0 ] || exit 1
[ "$skipped" = 0 ] || exit 2
exit 0
