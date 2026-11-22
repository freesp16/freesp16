#!/bin/sh
# Cross-build the device binary (armel, glibc 2.13) in the toolchain container, strip it, check its glibc symbol
# versions and dynamic dependencies. Output: out/armel/<name> (name from cmake/ProjectIdentity.cmake).
#   scripts/build-armel.sh          configure (first time) + build
#   scripts/build-armel.sh clean    wipe build/armel first
# TOOLCHAIN_IMAGE overrides the image tag (default sp16-toolchain:gcc11-egl, built from toolchain/ when missing); JOBS the
# parallel build jobs (default: every core of the container, $(nproc)).
set -eu
cd "$(dirname "$0")/.."
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
name=$(python3 tools/identity.py NAME)
[ -f third_party/JUCE/CMakeLists.txt ] || { echo "JUCE is missing: git submodule update --init --depth 1" >&2; exit 1; }
docker image inspect "$image" >/dev/null 2>&1 || {
  echo "building the toolchain image $image (GCC 11 + glibc 2.13 sysroot: about 30 minutes the first time)"
  docker build -t "$image" toolchain
}
[ "${1:-}" = clean ] && rm -rf build/armel
mkdir -p out/armel
# As the calling user (Linux Docker would otherwise leave build/armel and out/ owned by root; Docker Desktop on macOS
# maps ownership itself). HOME points into the build folder for tools that want a writable home.
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work/build/armel/.home -v "$PWD":/work -w /work "$image" sh -c "
  set -e
  [ -f build/armel/CMakeCache.txt ] || cmake --preset armel
  cmake --build --preset armel -j${JOBS:-\$(nproc)}
  cp build/armel/bin/$name out/armel/$name
  arm-linux-gnueabi-strip out/armel/$name
  file out/armel/$name
  sp16-check-glibc out/armel/$name
  if arm-linux-gnueabi-readelf -d out/armel/$name | grep -E 'NEEDED.*(EGL|GLES|GAL)'; then
    echo 'error: the binary links a GL library; it must bind the driver at run time (gldispatch)' >&2; exit 1
  fi
"
ls -l "out/armel/$name"
