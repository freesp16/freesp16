// Golden runner: one golden case per invocation (tests/golden/CMakeLists.txt registers one ctest per case, label
// "golden").
//
//   <prefix>_golden_runner <cases.json> <case-id> [--parity] [--fault gain] [--expect-failure] [--local-dir DIR]
//
//   --parity          bit-exact comparison instead of the tolerance (testing-strategy.md "Parity-exact mode")
//   --fault gain      run the subject with a deliberately broken gain (x (1 + 2^-10))
//   --expect-failure  exit 0 only when the comparison fails (with its report): proves a broken subject is caught
//   --local-dir DIR   local-only goldens (cases.json "local_only"; default <source>/build/golden-local or
//                     $APP_GOLDEN_LOCAL_DIR); a missing local golden is skipped (exit 77), never a failure
// Exit: 0 pass, 1 fail, 2 usage/load error, 77 skipped. On failure the actual output is written next to the build's
// artifacts (APP_ARTIFACT_DIR/golden/<name>/<case>.actual.f32) for inspection.
#include "tests/golden/harness/GoldenCase.h"
#include "tests/golden/harness/Subject.h"

#include "core/util/Realtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
int usage()
{
    std::fprintf(stderr, "usage: golden_runner <cases.json> <case-id> [--parity] [--fault gain] [--expect-failure] "
                         "[--local-dir DIR]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return usage();
    const fs::path recipe = argv[1];
    const std::string id = argv[2];
    bool parity = false, expectFailure = false;
    golden::Fault fault = golden::Fault::None;
    fs::path localDir;
    if (const char* env = std::getenv("APP_GOLDEN_LOCAL_DIR"))
        localDir = env;
#ifdef APP_SOURCE_DIR
    if (localDir.empty())
        localDir = fs::path(APP_SOURCE_DIR) / "build" / "golden-local";
#endif
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--parity")
            parity = true;
        else if (a == "--expect-failure")
            expectFailure = true;
        else if (a == "--fault" && i + 1 < argc && std::strcmp(argv[i + 1], "gain") == 0)
        {
            fault = golden::Fault::Gain;
            ++i;
        }
        else if (a == "--local-dir" && i + 1 < argc)
            localDir = argv[++i];
        else
            return usage();
    }

    juce::var doc;
    if (juce::JSON::parse(juce::File(juce::String(fs::absolute(recipe).string())).loadFileAsString(), doc).failed() ||
        ! doc.isObject())
    {
        std::fprintf(stderr, "GOLDEN ERROR: cannot read %s\n", recipe.string().c_str());
        return 2;
    }
    const std::string name = recipe.parent_path().filename().string();
    const bool local = static_cast<bool>(doc["local_only"]);
    const fs::path dir = local ? localDir / name : recipe.parent_path();
    const fs::path json = dir / (id + ".json");
    if (local && ! fs::exists(json))
    {
        std::printf("GOLDEN SKIP %s/%s: local-only golden not generated here (never committed, skipped in CI; "
                    "generate it with tools/golden-gen/gen.py %s and your own inputs)\n",
                    name.c_str(), id.c_str(), name.c_str());
        return 77;
    }

    golden::GoldenCase c;
    if (auto e = golden::loadCase(json, c); ! e.empty())
    {
        std::printf("GOLDEN FAIL %s/%s: %s\n", name.c_str(), id.c_str(), e.c_str());
        return expectFailure ? 2 : 1;
    }
    if (auto e = golden::checkGainLaw(c); ! e.empty())
    {
        std::printf("%s", e.c_str());
        return expectFailure ? 2 : 1;
    }
    auto subject = golden::makeSubject(c.subject);
    if (! subject)
    {
        std::printf("GOLDEN FAIL %s: unknown subject '%s' (tests/golden/harness/Subjects.cpp)\n", c.label().c_str(),
                    c.subject.c_str());
        return 1;
    }
    if (! subject->injectFault(fault))
    {
        std::printf("GOLDEN ERROR %s: subject %s has no such fault\n", c.label().c_str(), c.subject.c_str());
        return 2;
    }

    core::rt::enableFlushToZero(); // the app's audio-thread numerics (FX-055)
    golden::Channels actual;
    if (auto e = golden::runSubject(c, *subject, actual); ! e.empty())
    {
        std::printf("GOLDEN FAIL %s: %s\n", c.label().c_str(), e.c_str());
        return expectFailure ? 2 : 1;
    }
    std::string mode = parity ? "parity" : "";
    if (fault == golden::Fault::Gain)
        mode += std::string(mode.empty() ? "" : ", ") + "fault: gain";
    const auto result = golden::compare(c, actual, parity, mode);
    std::printf("%s", result.report.c_str());
    if (! result.pass)
    {
#ifdef APP_ARTIFACT_DIR
        const auto file =
            fs::path(APP_ARTIFACT_DIR) / "golden" / c.name /
            (c.id + (parity ? ".parity" : "") + (fault != golden::Fault::None ? ".fault" : "") + ".actual.f32");
        if (golden::writeF32(file, actual))
            std::printf("  actual output: %s (float32 LE, channel-major, as %s)\n", file.string().c_str(),
                        json.filename().string().c_str());
#endif
    }
    if (expectFailure)
    {
        if (result.pass)
        {
            std::printf("GOLDEN ERROR %s: the deliberately broken subject passed: the harness does not detect it\n",
                        c.label().c_str());
            return 1;
        }
        std::printf("GOLDEN OK %s: expected failure detected\n", c.label().c_str());
        return 0;
    }
    return result.pass ? 0 : 1;
}
