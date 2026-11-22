#!/bin/sh
# Build the NEON kernel tests for the device (armel, tests/neon/) in the toolchain container and run them under
# user-mode qemu as a Cortex-A9. On an x86-64 desktop this is the only place the NEON kernels are compared with their
# scalar references (the host suite SKIPs them). qemu timings mean nothing; A9 budgets are measured on hardware.
#   scripts/neon-tests.sh [catch2 args...]      e.g. scripts/neon-tests.sh '[simd]'
# Without arguments every test case except [params] runs: those check the parameter catalogue, whose static registrars
# are not linked here (plain link: the device GL renderer stays out); the host suites run them.
# Uses build/armel (the device build folder: shared objects, the test executable is EXCLUDE_FROM_ALL so the device
# build itself never builds it). TOOLCHAIN_IMAGE, JOBS as in scripts/build-armel.sh.
set -eu
cd "$(dirname "$0")/.."
[ $# -gt 0 ] || set -- "~[params]"
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
name=$(python3 tools/identity.py NAME)
[ -f third_party/JUCE/CMakeLists.txt ] || { echo "JUCE is missing: git submodule update --init --depth 1" >&2; exit 1; }
# Builds the image, or just adds the qemu layer to an existing one (the earlier layers come from the cache).
docker image inspect "$image" >/dev/null 2>&1 && docker run --rm "$image" sh -c 'command -v qemu-arm' >/dev/null 2>&1 \
  || docker build -t "$image" toolchain
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work/build/armel/.home -v "$PWD":/work -w /work "$image" sh -c "
  set -e
  [ -f build/armel/CMakeCache.txt ] || cmake --preset armel
  cmake -S . -B build/armel -DAPP_BUILD_NEON_TESTS=ON >/dev/null
  cmake --build build/armel --target ${name}_neon_tests -j${JOBS:-\$(nproc)}
  QEMU_CPU=cortex-a9 qemu-arm -L /opt/sp16/sysroot build/armel/bin/${name}_neon_tests --reporter console \"\$@\"
" neon-tests "$@"
