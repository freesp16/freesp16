// Force-included into every device bench source (tests/device-bench/CMakeLists.txt): APP_ARTIFACT_DIR — the scratch
// folder of the store benches (tests/unit/store/StoreTestSupport.h) and of the bench JSON results — is a run-time
// choice on the unit: $BENCH_TMPDIR, else /tmp/<name>-bench (RAM). Point BENCH_TMPDIR at the USB stick or the internal
// disk to time those benches on FAT instead of tmpfs.
#pragma once

const char* deviceBenchScratchDir() noexcept; // DeviceBenchMain.cpp
