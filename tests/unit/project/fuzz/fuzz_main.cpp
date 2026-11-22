// libFuzzer entry (NFR-011, host clang only). Inert in the test executables: a fuzz build compiles this file with
// -fsanitize=fuzzer,address,undefined and -DAPP_LIBFUZZER_TARGET=<n> together with FuzzTargets.cpp, the module
// libraries and the registrars (whole-archive), one binary per target:
//   1 project.json   2 meta.json   3 patterns/scene-NN.json   4 autosave copy   5 WAV/AIFF   6 .wav.dat/.wav.json
// e.g. ./fuzz_project_json -max_total_time=600 corpus/   (10 min per target locally, 60 s in CI).
#if defined(APP_LIBFUZZER_TARGET)
#include "FuzzTargets.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using projfuzz::BundleFile;
    switch (APP_LIBFUZZER_TARGET)
    {
    case 1: return projfuzz::bundleFile(BundleFile::Project, data, size);
    case 2: return projfuzz::bundleFile(BundleFile::Meta, data, size);
    case 3: return projfuzz::bundleFile(BundleFile::Scene, data, size);
    case 4: return projfuzz::autosave(data, size);
    case 5: return projfuzz::audio(data, size);
    default: return projfuzz::sidecar(data, size);
    }
}
#endif
