#!/bin/sh
# Run benchmarks as the device binary in the emulator (local only: needs your own imported firmware; the
# timings are relative only — qemu-arm user mode on a much faster host, no GPU — never A9 numbers).
#   scripts/emulator-bench.sh <bench>
#     <bench>  a bench file stem — tests/bench/<bench>.cpp, tests/bench/<bench>*.cpp or tests/bench/bench_<bench>*.cpp
#              (every BENCH in those files) — or the name of one BENCH (the file that defines it, that bench only)
#   1. build the harness + those sources standalone for armel (tests/bench/harness/device/, toolchain container),
#      linked against the module libraries of build/armel (run scripts/build-armel.sh first)
#   2. install it as the app (scripts/install.sh, into its own mod: $SP16_EMULATOR/private/<name>-bench), launch the
#      research build unattended (scripts/run.sh --bench)
#   3. wait for the "BENCH DONE" line in the session's console.txt, stop the emulator by its exact PID
#   4. print the [sp16-mod] BENCH lines; also saved to out/armel-bench/<bench>.txt and <session>/bench.txt
# Environment: SP16_EMULATOR, SP16_CUSTOM_APP (as install.sh), SP16_BUILD (default sp16-dev), TOOLCHAIN_IMAGE, JOBS
# (default 4), BENCH_TIMEOUT (seconds, default 900). Exit 0 = every selected bench ran.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
eval "$(python3 "$here/tools/identity.py" --shell)"
[ $# -eq 1 ] && [ -n "$1" ] || { echo "usage: $0 <bench file stem or BENCH name>" >&2; exit 2; }
arg=$1
case "$arg" in *[!A-Za-z0-9_/-]*|/*|*..*) echo "bench names contain only letters, digits, '_', '-', '/'" >&2; exit 2;; esac
emu=${SP16_EMULATOR:-$HOME/src/sp16-emulator}
build=${SP16_BUILD:-sp16-dev}
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
prefix=$(echo "$PROJECT_NAME" | tr '-' '_')
tag="[sp16-mod] $PROJECT_ENV_PREFIX"
mod=$emu/private/$PROJECT_NAME-bench
cd "$here"

# 1. Sources and the run-time filter.
sources=$(for f in tests/bench/"$arg".cpp tests/bench/"$arg"*.cpp tests/bench/bench_"$arg"*.cpp; do
  [ -f "$f" ] && echo "$f"; done | sort -u)
filter=""
if [ -z "$sources" ]; then
  sources=$(grep -rlE "^[[:space:]]*BENCH\([[:space:]]*$arg[[:space:]]*," tests/bench --include='*.cpp' \
            | grep -v '^tests/bench/harness/' || true)
  filter=$arg
fi
[ -n "$sources" ] || { echo "no bench file or BENCH named '$arg' under tests/bench" >&2; exit 2; }
if ! grep -qE '^[[:space:]]*BENCH\(' $sources; then
  echo "$arg: $(echo $sources) define no BENCH() (Catch2-only benches run on the host: ctest --preset host-release -L bench)" >&2
  exit 2
fi
[ -f "build/armel/src/engine/lib${prefix}_engine.a" ] || { echo "no armel build: run scripts/build-armel.sh first" >&2; exit 1; }
list=$(echo $sources | tr ' ' ';')
echo "bench sources: $(echo $sources)${filter:+ (filter $filter)}"

mkdir -p out/armel-bench
bin=out/armel-bench/${prefix}_device_bench
docker run --rm -v "$PWD":/work -w /work "$image" sh -c "
  set -e
  cmake -S tests/bench/harness/device -B build/armel-bench -G Ninja -DCMAKE_TOOLCHAIN_FILE=/work/toolchain/cross.cmake \
        -DCMAKE_BUILD_TYPE=Release '-DAPP_BENCH_SOURCES=$list' >/dev/null
  cmake --build build/armel-bench -j${JOBS:-4}
  cp build/armel-bench/bin/${prefix}_device_bench $bin
  arm-linux-gnueabi-strip $bin
  sp16-check-glibc $bin
"
ls -l "$bin"

# 2. Install as the app (install.sh installs out/armel/<name>: the real app is moved aside and put back).
if pgrep -f "build/$build/.*MacOS/SP16Emulator" >/dev/null; then
  echo "an emulator session of $build is already running; stop it first (kill <its pid>)" >&2; exit 2
fi
app=out/armel/$PROJECT_NAME
saved=out/armel/.$PROJECT_NAME.during-bench
pid=""
stop() {
  [ -n "$pid" ] || return 0
  kill "$pid" 2>/dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  pid=""
}
cleanup() { stop; [ -f "$saved" ] && mv -f "$saved" "$app"; return 0; }
trap cleanup EXIT INT TERM
mkdir -p out/armel
[ -f "$app" ] && mv -f "$app" "$saved"
cp "$bin" "$app"
APP_MOD=$mod "$here/scripts/install.sh" "bench=$filter"
rm -f "$app"
[ -f "$saved" ] && mv -f "$saved" "$app"

# 3. Run, wait for BENCH DONE, stop by exact PID.
out=$(APP_MOD=$mod SP16_BUILD=$build "$here/scripts/run.sh" --bench)
echo "$out"
session=$(echo "$out" | sed -n 's/^session: //p')
pid=$(echo "$out" | sed -n 's/^emulator pid: //p')
case "$pid" in ''|unknown|*[!0-9]*) pid=""; echo "could not find the emulator PID" >&2; exit 2;; esac
timeout=${BENCH_TIMEOUT:-900}
echo "waiting for the benches (BENCH DONE in $session/console.txt, at most ${timeout} s)..."
waited=0
while [ "$waited" -lt "$timeout" ]; do
  grep -qF "$tag BENCH DONE" "$session/console.txt" 2>/dev/null && break
  kill -0 "$pid" 2>/dev/null || { echo "the emulator exited" >&2; break; }
  sleep 2; waited=$((waited + 2))
done
sleep 2 # let the bridge copy the last lines
stop
echo "emulator stopped"

# 4. Report.
report=out/armel-bench/$(echo "$arg" | tr '/' '_').txt
grep -F "$tag BENCH" "$session/console.txt" | sed "s/^.*$PROJECT_ENV_PREFIX BENCH/BENCH/" > "$report" || true
cp "$report" "$session/bench.txt"
echo "--- device benches (emulator: relative timings only) — $report"
cat "$report"
grep -q "^BENCH DONE ran=[1-9][0-9]* over=[0-9]* failed=0" "$report" || {
  echo "the benches did not finish (see $session/console.txt)" >&2; exit 1; }
