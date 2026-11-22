// Test rig of the step sequencer (tests only): an engine::Engine bound to the built-in registry, patterns built from
// the model through the same path the app uses (makePatternImage → the registered engine builder → ObjectSwap), and a
// block-by-block driver that records every event the trigger router applied, with its absolute frame.
#pragma once

#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/Pattern.h"
#include "core/params/ParamRegistry.h"
#include "core/util/sequencer/PatternImage.h"
#include "engine/voice/VoiceSwaps.h"

#include <memory>
#include <vector>

namespace seqtest
{
using engine::trigger::TriggerKind;
using engine::trigger::TriggerSource;

inline core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

struct Hit
{
    long frame; // absolute frame from the first rendered block
    int track;
    TriggerKind kind;
    TriggerSource source;
    int velocity;
    bool locked;
    bool ducks;
};

inline std::unique_ptr<core::model::Pattern> pattern(int length = 16)
{
    auto p = std::make_unique<core::model::Pattern>();
    p->length = length;
    return p;
}

// Writes a trigger straight into a model pattern (the commands are tested on their own).
inline void put(core::model::Pattern& p, int step, int track, core::model::TriggerType type = core::model::TriggerType::Full,
                int velocity = 127)
{
    auto& c = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    c.trigger = type == core::model::TriggerType::Full ? 15 : (type == core::model::TriggerType::Half ? 11 : 8);
    c.triggerType = type;
    c.value2 = static_cast<std::int16_t>(velocity);
}
inline void lock(core::model::Pattern& p, int step, int track, int slot, float value,
                 core::model::LockChange change = core::model::LockChange::Revert)
{
    auto& c = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    if (c.lockIndex == core::model::Step::kNoLocks)
    {
        p.locks.emplace_back();
        c.lockIndex = static_cast<std::uint16_t>(p.locks.size() - 1);
    }
    auto& l = p.locks[c.lockIndex];
    l.mask |= std::uint64_t {1} << slot;
    l.values[static_cast<std::size_t>(slot)] = value;
    l.change[static_cast<std::size_t>(slot)] = change;
}

struct Rig
{
    std::unique_ptr<engine::Engine> e;
    long frame = 0;
    std::vector<Hit> hits;
    std::vector<float> mainL; // MAIN L of every rendered block

    explicit Rig(bool testTone = false, double rate = 44100.0) : e(enginetest::make(testTone, rate, 512)) {}
    // With the composition root's full set-up (the FX slot bank routed).
    struct Full
    {
    };
    explicit Rig(Full, double rate = 44100.0)
        : e(std::make_unique<engine::Engine>(enginetest::fullSetup(), engine::EngineOptions {}))
    {
        e->prepare(rate, 512);
    }

    bool install(const core::model::Pattern& p, int scene = 0, int pat = 0)
    {
        auto image = std::make_unique<core::sequencer::PatternImage>();
        core::commands::sequencer::makePatternImage(p, *image);
        const auto c = core::sequencer::patternCompiler();
        const void* obj = c.build(*image);
        const engine::SwapSlot slot {engine::SwapSlot::Pattern, static_cast<std::uint8_t>(scene), static_cast<std::uint8_t>(pat)};
        if (e->bridge().post(engine::ObjectSwap {slot, obj}))
            return true;
        c.destroy(obj);
        return false;
    }
    bool param(const char* name, float v, std::uint16_t scope = 0)
    {
        return e->bridge().post(engine::ParamChange {id(name), scope, v, 0});
    }
    bool play() { return e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}); }
    bool play(int preCountBars) { return e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, double(preCountBars)}); }
    bool resume() { return e->bridge().post(engine::TransportCmd {engine::TransportCmd::Continue, 0.0}); }
    bool locate(double ppq) { return e->bridge().post(engine::TransportCmd {engine::TransportCmd::Locate, ppq}); }
    // The switcher's scene.queue: switch to (scene, pattern) at the next boundary (immediately while stopped).
    bool queue(int scene, int pat) { return param("scene.queue", static_cast<float>(scene * 16 + pat), 0xFFFF); }
    bool stop() { return e->bridge().post(engine::TransportCmd {engine::TransportCmd::Stop, 1.0}); } // STOP: step 1
    bool pause() { return e->bridge().post(engine::TransportCmd {engine::TransportCmd::Stop, 0.0}); }

    // One 64-frame block (real-time thread), then the router's applied events are recorded.
    void block()
    {
        float l[engine::kBlockFrames], r[engine::kBlockFrames];
        float* const out[2] = {l, r};
        {
            core::rt::ScopedRealtimeThread realtime;
            e->process(out, 2, engine::kBlockFrames);
        }
        mainL.insert(mainL.end(), l, l + engine::kBlockFrames);
        for (const auto& ev : e->triggerRouter().blockEvents())
            hits.push_back({frame + ev.frame, ev.track, ev.kind, ev.source, ev.velocity, ev.locks != nullptr, ev.ducks});
        frame += engine::kBlockFrames;
    }
    void blocks(long n)
    {
        for (long i = 0; i < n; ++i)
            block();
    }
    // Renders until `absFrame` is inside the rendered range.
    void until(long absFrame)
    {
        while (frame <= absFrame)
            block();
    }
    std::vector<Hit> sequenced(int track = -1) const
    {
        std::vector<Hit> v;
        for (const auto& h : hits)
            if (h.source == TriggerSource::Sequencer && (track < 0 || h.track == track))
                v.push_back(h);
        return v;
    }
    std::vector<engine::EngineEvent> events()
    {
        std::vector<engine::EngineEvent> v;
        engine::EngineEvent ev;
        while (e->bridge().popEvent(ev))
            v.push_back(ev);
        return v;
    }
};

// Frames of step n at `bpm` (44.1 kHz): n × 60 · 44100 / (4 · bpm).
inline double stepFrames(double bpm = 120.0) { return 60.0 * 44100.0 / (4.0 * bpm); }
} // namespace seqtest
