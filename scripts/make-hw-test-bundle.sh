#!/bin/sh
# The hardware-test bundle for the first full run on the reference hardware (docs/hardware-test.md):
# out/hw-test/ with the generated test kit laid out for the USB stick, the settings variants, the input scripts of the
# unattended self-test and the human-led checks, the unit-side helpers (use.sh, report.sh) and the PC-side helper
# (pc/hwt.sh). Everything is generated (tools/hw-test-bundle.py, tools/gen-test-kit.py, tools/liveset-check.py): no
# firmware, keys, captures or device data.
#   scripts/make-hw-test-bundle.sh                 LIVESET_MINUTES (default 30), LIVESET_SEED (default 1604)
# The app and the bench binaries are separate: scripts/build-armel.sh, scripts/build-armel-bench.sh.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/out/hw-test"
rm -rf "$out/stick" "$out/pc"
mkdir -p "$out"
python3 "$here/tools/hw-test-bundle.py" make --out "$out" --liveset-minutes "${LIVESET_MINUTES:-30}" \
  --seed "${LIVESET_SEED:-1604}"
name=$(python3 "$here/tools/identity.py" NAME)
echo
echo "next:"
echo "  1. copy the CONTENT of $out/stick/ onto the USB stick's root (beside the launcher's autoexec image)"
echo "  2. scripts/build-armel.sh (out/armel/$name) and $out/pc/hwt.sh deploy"
echo "  3. follow docs/hardware-test.md: $out/pc/hwt.sh use 01-baseline, then $out/pc/hwt.sh report baseline"
for f in out/armel/"$name" out/armel/"$name"_bench; do
  [ -f "$here/$f" ] && echo "  ($f: $(ls -l "$here/$f" | awk '{print $5}') bytes, $(date -r "$here/$f" '+%F %T'))"
done
exit 0
