// Library test fixture: a temp library root, workers, pool, and an EngineBridge fake that owns installed ObjectSwaps
// the way the engine does (one reference per installed slot, released when replaced — the engine's TrackSample and
// Preview destroy functions release on the message thread after the retire grace).
#pragma once

#include "LibTestAudio.h"

#include "core/audio/SampleBuffer.h"
#include "core/model/Project.h"
#include "engine/EngineApi.h"
#include "services/library/SampleLibrary.h"
#include "services/library/SamplePool.h"
#include "services/library/Workers.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <vector>

namespace libtest
{
class SwapEngine final : public engine::EngineBridge
{
public:
    ~SwapEngine() override
    {
        for (auto& [k, p] : installed)
            core::audio::release(static_cast<const core::audio::SampleBuffer*>(p));
    }
    bool post(const engine::EngineMessage& m) override
    {
        if (! accept)
            return false;
        ++posts;
        if (const auto* s = std::get_if<engine::ObjectSwap>(&m))
        {
            const int key = static_cast<int>(s->slot.kind) * 256 + s->slot.scene * 16 + s->slot.index;
            const void* old = installed[key];
            installed[key] = s->object;
            swaps.push_back(*s);
            core::audio::release(static_cast<const core::audio::SampleBuffer*>(old)); // retire → destroy
        }
        return true;
    }
    void apply(const core::ChangeSet&, const engine::model::Project&) override {}
    const engine::PlayheadFeed& playhead() const override { return feed; }
    const engine::MeterFeed& meters() const override { return meter; }
    void retirePending() override {}
    bool popEvent(engine::EngineEvent&) override { return false; }

    const core::audio::SampleBuffer* track(int scene, int t) const
    {
        const auto it = installed.find(scene * 16 + t);
        return it == installed.end() ? nullptr : static_cast<const core::audio::SampleBuffer*>(it->second);
    }
    const core::audio::SampleBuffer* preview() const
    {
        const auto it = installed.find(256);
        return it == installed.end() ? nullptr : static_cast<const core::audio::SampleBuffer*>(it->second);
    }

    std::map<int, const void*> installed;
    std::vector<engine::ObjectSwap> swaps;
    int posts = 0;
    bool accept = true;
    engine::PlayheadFeed feed{};
    engine::MeterFeed meter{};
};

// Writes a generated sample (mono 16-bit sine unless told otherwise) and returns the file.
inline juce::File writeSample(const juce::File& file, std::int64_t frames = 4410, double hz = 440, int channels = 1,
                              double rate = 44100)
{
    Spec s;
    s.channels = channels;
    s.rate = rate;
    std::vector<std::vector<float>> src;
    for (int c = 0; c < channels; ++c)
        src.push_back(sine(frames, hz * (c + 1), rate));
    writeFile(file, encode(s, src));
    return file;
}

struct Fixture
{
    explicit Fixture(services::library::SamplePool::Config config = {})
        : library(workers),
          pool(workers, [this](const std::string& p) { return library.resolve(juce::String(p)); }, config)
    {
        REQUIRE(library.setRoot(tmp.dir().getChildFile("Lib")).ok());
    }
    ~Fixture() { workers.drain(); }

    juce::File samples() const { return library.root().getChildFile("Samples"); }

    TempDir tmp{"pool"};
    services::library::Workers workers{2};
    services::library::SampleLibrary library;
    services::library::SamplePool pool;
};
} // namespace libtest
