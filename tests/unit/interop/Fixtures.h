// Synthetic reference projects for the interop tests (clean room and data safety): every input is generated
// at test time by tools/prj-dump/prj-synth.py from a JSON description in tests/unit/interop/synthetic/
// into a temporary folder that is removed at exit. No device file is ever read; nothing is written to the repository.
#pragma once

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace interop_test
{
inline juce::File sourceDir() { return juce::File(APP_SOURCE_DIR); }

class TempRoot
{
public:
    TempRoot()
        : dir_(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("interop-test-" + juce::String(juce::Time::currentTimeMillis()) + "-" +
                                 juce::String(juce::Random::getSystemRandom().nextInt(1 << 30))))
    {
        dir_.createDirectory();
    }
    ~TempRoot() { dir_.deleteRecursively(); }
    const juce::File& dir() const noexcept { return dir_; }

private:
    juce::File dir_;
};

inline TempRoot& temp()
{
    static TempRoot t;
    return t;
}

inline bool runTool(const juce::StringArray& args, juce::String* output = nullptr)
{
    juce::ChildProcess p;
    if (! p.start(args))
        return false;
    const auto out = p.readAllProcessOutput();
    if (! p.waitForProcessToFinish(120000))
        return false;
    if (output != nullptr)
        *output = out;
    return p.getExitCode() == 0;
}

inline bool havePython()
{
    static const bool ok = runTool({"python3", "-c", "import zlib"});
    return ok;
}

// The generated disk tree of one description: <root>/PIONEER DJ SAMPLER/{Projects/<name>.prj, Samples/…, System/…}.
struct Generated
{
    juce::File out;      // the folder passed to prj-synth
    juce::File diskRoot; // out/PIONEER DJ SAMPLER
    juce::File prj;
};

// Generates (once per run) the fixture `name` (tests/unit/interop/synthetic/<name>.json); nullopt = python3 missing.
inline std::optional<Generated> fixture(const std::string& name)
{
    static std::map<std::string, Generated> cache;
    if (auto it = cache.find(name); it != cache.end())
        return it->second;
    if (! havePython())
        return std::nullopt;
    Generated g;
    g.out = temp().dir().getChildFile(name);
    g.out.deleteRecursively();
    const auto desc = sourceDir().getChildFile("tests/unit/interop/synthetic").getChildFile(name + ".json");
    juce::String text;
    const bool ok = runTool({"python3", sourceDir().getChildFile("tools/prj-dump/prj-synth.py").getFullPathName(),
                             desc.getFullPathName(), g.out.getFullPathName()},
                            &text);
    INFO(text.toStdString());
    REQUIRE(ok);
    g.prj = juce::File(text.trim().fromLastOccurrenceOf("\n", false, false).trim());
    if (! g.prj.existsAsFile())
        g.prj = juce::File(text.trim());
    REQUIRE(g.prj.existsAsFile());
    g.diskRoot = g.prj.getParentDirectory().getParentDirectory();
    cache.emplace(name, g);
    return g;
}

inline std::vector<std::byte> bytesOf(const juce::File& f)
{
    juce::MemoryBlock mb;
    REQUIRE(f.loadFileAsData(mb));
    const auto* d = static_cast<const std::byte*>(mb.getData());
    return {d, d + mb.getSize()};
}

#define INTEROP_FIXTURE(var, name)                                                                                     \
    const auto var##Opt = ::interop_test::fixture(name);                                                               \
    if (! var##Opt)                                                                                                    \
        SKIP("python3 not available: the synthetic fixtures cannot be generated");                                    \
    const auto& var = *var##Opt
} // namespace interop_test
