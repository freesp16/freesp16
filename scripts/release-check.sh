#!/bin/sh
# Release checklist: runs everything that has to pass before a merge or a release, in order, and prints
# one table (stage · result · time · log). Each stage's output goes to out/release-check/<timestamp>/<stage>.log.
#   scripts/release-check.sh                       every stage except soak and long
#   scripts/release-check.sh --only lint,host-fast  just those      --skip neon,armel   all but those
#   scripts/release-check.sh --soak-hours 1         adds the desktop soak (tools/soak/soak-run.py run, + kill test)
#   scripts/release-check.sh --long                 adds the [long] tests under ASan (1,000 fork-and-kill saves, …)
# Stages (the commands are the ones of ci/run-local.sh and the merge checklist):
#   lint        tools/check-identity.sh, tools/check-no-firmware.sh, tools/check-deps.py, tools/rt-lint.py
#   host-fast   cmake --preset host-fast, build, ctest --preset host-fast (Release + RT allocation hook)
#   host        cmake --preset host, build, ctest --preset host --label-exclude "golden|long" (Debug + ASan/UBSan)
#   golden      ctest --preset host -L golden (needs ../PioneerSP16FirmwareResearch; SKIPs without it)
#   armel       scripts/build-armel.sh (toolchain container: GCC 11, glibc 2.13 sysroot)      SKIP without docker
#   glibc       tools/soak/check-glibc.py out/armel/<name> (symbol versions <= 2.13, static libstdc++, no GL in
#               DT_NEEDED, soft-float EABI) + the container's sp16-check-glibc when docker is there
#   neon        scripts/neon-tests.sh (NEON kernels vs scalar references under qemu-arm -cpu cortex-a9)  SKIP w/o docker
#   host-smoke  scripts/host-smoke.sh (Xvfb end-to-end: tone, kit, every scripts/smoke/ check)    SKIP w/o xvfb-run
#   soak        tools/soak/soak-run.py run --hours H (only with --soak-hours)
#   long        ctest --preset host -L long (only with --long)
# Environment: JOBS (build parallelism, default 8), RENDERS (host-smoke).
# Exit: 0 = no stage failed (skips allowed), 1 = a stage failed, 2 = bad arguments.
set -u
cd "$(dirname "$0")/.."
here=$(pwd)
name=$(python3 tools/identity.py NAME)
jobs=${JOBS:-8}
all="lint host-fast host golden armel glibc neon host-smoke"
only="" skip="" soak_hours="" long=0
while [ $# -gt 0 ]; do
  case "$1" in
    --only) only=$(echo "$2" | tr ',' ' '); shift 2 ;;
    --skip) skip=$(echo "$2" | tr ',' ' '); shift 2 ;;
    --soak-hours) soak_hours=$2; shift 2 ;;
    --long) long=1; shift ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "release-check: unknown argument $1" >&2; exit 2 ;;
  esac
done
stages=${only:-$all}
[ -n "$soak_hours" ] && [ -z "$only" ] && stages="$stages soak"
[ "$long" = 1 ] && [ -z "$only" ] && stages="$stages long"
for s in $stages; do
  case " $all soak long " in *" $s "*) ;; *) echo "release-check: unknown stage $s (one of: $all soak long)" >&2; exit 2 ;; esac
done
out="$here/out/release-check/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
table="$out/table.txt"
: > "$table"
failed=0
have() { command -v "$1" >/dev/null 2>&1; }

# run_stage <stage> <command...>: PASS / FAIL by exit code; 77 = SKIP (the command says why on its last line)
run_stage() {
  st=$1; shift
  log="$out/$st.log"
  printf '==== %-10s ' "$st"
  t0=$(date +%s)
  ( "$@" ) > "$log" 2>&1
  code=$?
  dt=$(( $(date +%s) - t0 ))
  case $code in
    0) res=PASS ;;
    77) res=SKIP ;;
    *) res=FAIL; failed=1 ;;
  esac
  note=""
  [ "$res" = PASS ] || note=$(grep -v '^[[:space:]]*$' "$log" | tail -n 1 | cut -c1-90)
  printf '%s (%ss)\n' "$res" "$dt"
  printf '%-10s  %-4s  %6ss  %s  %s\n' "$st" "$res" "$dt" "${log#$here/}" "$note" >> "$table"
}

skip_with() { echo "SKIP: $*"; exit 77; }

st_lint() {
  set -e
  tools/check-identity.sh
  tools/check-no-firmware.sh
  python3 tools/check-deps.py
  python3 tools/rt-lint.py
}
st_host_fast() {
  set -e
  cmake --preset host-fast
  cmake --build --preset host-fast -j"$jobs"
  ctest --preset host-fast -j"$jobs"
}
st_host() {
  set -e
  cmake --preset host
  cmake --build --preset host -j"$jobs"
  ctest --preset host -j"$jobs" --label-exclude "golden|long"
}
st_golden() {
  [ -d ../PioneerSP16FirmwareResearch ] || skip_with "../PioneerSP16FirmwareResearch not found"
  set -e
  cmake --preset host >/dev/null
  cmake --build --preset host -j"$jobs"
  ctest --preset host -L golden --no-tests=ignore
}
st_armel() {
  have docker || skip_with "docker not found (the toolchain container builds the device binary)"
  JOBS=$jobs scripts/build-armel.sh
}
st_glibc() {
  [ -f "out/armel/$name" ] || skip_with "no out/armel/$name (run the armel stage first)"
  set -e
  python3 tools/soak/check-glibc.py "out/armel/$name"
  if have docker && docker image inspect "${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}" >/dev/null 2>&1; then
    docker run --rm -v "$PWD":/work -w /work "${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}" sp16-check-glibc "out/armel/$name"
  fi
}
st_neon() {
  have docker || skip_with "docker not found (NEON parity runs under qemu in the toolchain container)"
  JOBS=$jobs scripts/neon-tests.sh
}
st_host_smoke() {
  have xvfb-run || skip_with "xvfb-run not found"
  [ -x "build/host-fast/bin/$name" ] || skip_with "no build/host-fast/bin/$name (run host-fast first)"
  scripts/host-smoke.sh
}
st_soak() {
  [ -x "build/host-fast/bin/$name" ] || skip_with "no build/host-fast/bin/$name (run host-fast first)"
  python3 tools/soak/soak-run.py run --hours "$soak_hours" --out "$out/soak"
}
st_long() {
  set -e
  cmake --preset host >/dev/null
  cmake --build --preset host -j"$jobs"
  ctest --preset host -L long
}

echo "release-check: stages: $stages"
echo "logs: ${out#$here/}"
for s in $stages; do
  fn=st_$(echo "$s" | tr '-' '_')
  case " $skip " in
    *" $s "*) printf '%-10s  %-4s  %6s   %s\n' "$s" "skip" "-" "(--skip)" >> "$table"; continue ;;
  esac
  run_stage "$s" "$fn"
done
echo
printf '%-10s  %-4s  %7s  %s\n' "stage" "res" "time" "log  (last line when not PASS)"
cat "$table"
commit=$(git rev-parse --short HEAD 2>/dev/null || echo "?")
dirty=$(git status --porcelain 2>/dev/null | grep -v '^??' | head -n 1)
echo "commit $commit${dirty:+ (uncommitted changes)}"
if [ "$failed" = 0 ]; then echo "RELEASE CHECK PASS"; else echo "RELEASE CHECK FAIL"; fi
exit $failed
