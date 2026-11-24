#!/bin/sh
# Run the CI stages locally, in the same order and with the same commands as .github/workflows/ci.yml.
#   ci/run-local.sh [stage...]     stages: lint host bench golden armel glibc package   (default: all of them)
#   ci/run-local.sh smoke          local only (needs your own firmware in sp16-emulator): scripts/emulator-smoke.sh
#   ci/run-local.sh long           the tests labelled long (minutes each, e.g. the 1,000 fork-and-kill saves); host skips them
set -eu
cd "$(dirname "$0")/.."
stages=${*:-lint host bench golden armel glibc package}
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
eval "$(python3 tools/identity.py --shell)"
step() { printf '\n==== %s\n' "$*"; }

for stage in $stages; do
  case "$stage" in
    lint)
      step "lint: identity, no-firmware, dependency rules, real-time lint, format"
      tools/check-identity.sh
      tools/check-no-firmware.sh
      python3 tools/check-deps.py   # module dependency rules (spec 01-architecture/module-layout.md)
      python3 tools/rt-lint.py      # real-time rules for every REALTIME file (engine/**)
      if command -v clang-format >/dev/null; then
        git ls-files '*.cpp' '*.h' | grep -v '^third_party/' | xargs clang-format --dry-run --Werror
      else
        echo "(clang-format not installed: format check skipped locally; CI runs it)"
      fi;;
    host)
      step "host: Debug + ASan/UBSan + real-time allocation hook, unit tests"
      cmake --preset host
      cmake --build --preset host ${JOBS:+-j"$JOBS"}
      ctest --preset host --label-exclude "golden|long";;
    bench)
      step "bench: Release benchmarks vs their performance budgets (3 runs, fastest kept), > 10 % regression fails"
      cmake --preset host-release
      cmake --build --preset host-release ${JOBS:+-j"$JOBS"}
      rm -rf build/host-release/artifacts/bench
      for run in 1 2 3; do
        echo "bench run $run/3"
        ctest --preset host-release -L bench
      done
      python3 tests/bench/harness/check_regression.py;;   # vs ci/bench-baseline.json (same machine only)
    golden)
      step "golden: effect/mixer goldens from the research models (needs ../PioneerSP16FirmwareResearch)"
      cmake --preset host >/dev/null
      cmake --build --preset host
      ctest --preset host -L golden --no-tests=ignore;;
    armel)
      step "armel: cross-build in the toolchain container"
      scripts/build-armel.sh;;
    glibc)
      step "glibc: symbol versions <= 2.13 and no GL library in DT_NEEDED"
      docker run --rm -v "$PWD":/work -w /work "$image" sp16-check-glibc "out/armel/$PROJECT_NAME"
      ! docker run --rm -v "$PWD":/work -w /work "$image" arm-linux-gnueabi-readelf -d "out/armel/$PROJECT_NAME" \
        | grep -E 'NEEDED.*(EGL|GLES|GAL)';;
    package)
      step "package: device tarball + emulator mod"
      scripts/package.sh;;
    long)
      step "long: the tests labelled long (excluded from host)"
      cmake --preset host
      cmake --build --preset host ${JOBS:+-j"$JOBS"}
      ctest --preset host -L long;;
    smoke)
      step "smoke: emulator end-to-end (local only)"
      scripts/emulator-smoke.sh;;
    *) echo "unknown stage $stage" >&2; exit 2;;
  esac
done
printf '\nci/run-local.sh: %s passed\n' "$stages"
