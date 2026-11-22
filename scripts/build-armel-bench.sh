#!/bin/sh
# Cross-build the device bench executable (armel, glibc 2.13; tests/device-bench/) in the toolchain container, strip
# it and check its glibc symbol versions and dynamic dependencies like the app (scripts/build-armel.sh).
# Output: out/armel/<name>_bench (name from cmake/ProjectIdentity.cmake). Run it on a unit over SSH (docs/
# hardware-test.md "Device benches") and turn its output into a table with tools/bench-report.py.
#   scripts/build-armel-bench.sh                     configure build/armel (first time) + build the bench
#   scripts/build-armel-bench.sh --qemu [args...]    … then run it under qemu-arm as a Cortex-A9 in the container
#                                                    (it runs; qemu timings mean nothing) — args go to Catch2, e.g.
#                                                    '[mixer]' or 'bench_voice*'; output also in out/armel/bench-qemu.txt
# Uses build/armel (shared with scripts/build-armel.sh and scripts/neon-tests.sh; the bench target is EXCLUDE_FROM_ALL
# so the app build never builds it). Catch2 comes from build/host-fast/_deps/catch2-src when that exists (no network
# needed), else it is fetched. TOOLCHAIN_IMAGE, JOBS as in scripts/build-armel.sh; BENCH_QEMU_TIMEOUT (seconds,
# default 3600) bounds the qemu run.
set -eu
cd "$(dirname "$0")/.."
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
name=$(python3 tools/identity.py NAME)
prefix=$(echo "$name" | tr '-' '_')
qemu=0
if [ "${1:-}" = --qemu ]; then qemu=1; shift; fi
[ -f third_party/JUCE/CMakeLists.txt ] || { echo "JUCE is missing: git submodule update --init --depth 1" >&2; exit 1; }
docker image inspect "$image" >/dev/null 2>&1 || {
  echo "building the toolchain image $image (GCC 11 + glibc 2.13 sysroot: about 30 minutes the first time)"
  docker build -t "$image" toolchain
}
if [ "$qemu" = 1 ] && ! docker run --rm "$image" sh -c 'command -v qemu-arm' >/dev/null 2>&1; then
  docker build -t "$image" toolchain # adds the qemu layer (the earlier layers come from the cache)
fi
catch=""
[ -f build/host-fast/_deps/catch2-src/CMakeLists.txt ] && catch="-DFETCHCONTENT_SOURCE_DIR_CATCH2=/work/build/host-fast/_deps/catch2-src"
mkdir -p out/armel
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work/build/armel/.home -v "$PWD":/work -w /work "$image" sh -c "
  set -e
  [ -f build/armel/CMakeCache.txt ] || cmake --preset armel
  cmake -S . -B build/armel -DAPP_BUILD_DEVICE_BENCH=ON $catch >/dev/null
  cmake --build build/armel --target ${prefix}_device_benches -j${JOBS:-\$(nproc)}
  cp build/armel/bin/${name}_bench out/armel/${name}_bench
  arm-linux-gnueabi-strip out/armel/${name}_bench
  file out/armel/${name}_bench
  sp16-check-glibc out/armel/${name}_bench
  if arm-linux-gnueabi-readelf -d out/armel/${name}_bench | grep -E 'NEEDED.*(EGL|GLES|GAL)'; then
    echo 'error: the bench links a GL library; it must bind the driver at run time (gldispatch)' >&2; exit 1
  fi
  arm-linux-gnueabi-readelf -d out/armel/${name}_bench | sed -n 's/.*NEEDED.*\[\(.*\)\]/needed: \1/p'
"
ls -l "out/armel/${name}_bench"
[ "$qemu" = 1 ] || exit 0
echo "--- qemu-arm (Cortex-A9) run: timings are meaningless here, this only shows that it runs"
set +e
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work -w /work "$image" sh -c "
  QEMU_CPU=cortex-a9 timeout ${BENCH_QEMU_TIMEOUT:-3600} qemu-arm -L /opt/sp16/sysroot out/armel/${name}_bench \"\$@\"
" bench "$@" > out/armel/bench-qemu.txt 2>&1
rc=$?
set -e
tail -n 25 out/armel/bench-qemu.txt
echo "qemu run exit code $rc (output: out/armel/bench-qemu.txt)"
exit $rc
