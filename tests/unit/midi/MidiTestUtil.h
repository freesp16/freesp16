// Helpers for the MIDI unit tests: an engine bound to the built-in registry (with the FX bank), the CC
// table on its hub, synthetic host time (the offline driver has no device clock: host ns = frame · 1e9 / rate), and
// a block-by-block driver that records the router's MIDI-sourced events with their absolute frame.
#pragma once

#include "engine/Engine.h"
#include "engine/fx/FxSlotBank.h"

#include "core/audio/SampleBufferStorage.h"
#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "core/util/midi/CcTable.h"
#include "io/midi/CcTableBuilder.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace miditest
{
inline core::ParamId id(std::string_view name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

inline const core::midi::CcTable& table()
{
    static const core::midi::CcTable t = [] {
        core::midi::CcTable x;
        io::midi::buildCcTable(core::builtinParams(), x);
        return x;
    }();
    return t;
}

inline std::unique_ptr<engine::Engine> make(double rate = 44100.0)
{
    engine::EngineSetup s {[](std::string_view i) { return core::builtinParams().idOf(i).value_or(core::ParamId {}); }};
    for (const auto& spec : core::builtinParams().all())
        s.paramIds.push_back(spec.id);
    s.fxInternalIndex = core::fx::internalIndexTable(core::builtinParams());
    engine::EngineOptions o;
    o.testTone = false;
    auto e = std::make_unique<engine::Engine>(s, o);
    e->prepare(rate, 2048);
    e->midi().hub().setCcTable(&table());
    return e;
}

inline std::int64_t hostNs(double frame, double rate = 44100.0)
{
    return static_cast<std::int64_t>(std::llround(frame * 1.0e9 / rate));
}

inline core::midi::MidiEvent msg(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, std::int64_t ns, int port = 0)
{
    core::midi::MidiEvent e;
    e.port = static_cast<std::uint8_t>(port);
    e.size = static_cast<std::uint8_t>(b0 >= 0xF8 ? 1 : ((b0 & 0xF0) == 0xC0 || (b0 & 0xF0) == 0xD0 ? 2 : 3));
    e.bytes[0] = b0;
    e.bytes[1] = b1;
    e.bytes[2] = b2;
    e.hostTimeNs = ns;
    return e;
}

struct MidiHit
{
    std::int64_t frame; // absolute engine frame
    int track, semitones, velocity;
    engine::trigger::TriggerKind kind;
};

// Drives the engine in device periods of `period` frames (offline: no device clock) and collects, after every block,
// the router's MIDI-sourced events. Needs period to be a multiple of 64 when hits are collected per block.
struct Driver
{
    Driver(engine::Engine& engine, int periodFrames) : e(engine), period(periodFrames) {}
    engine::Engine& e;
    int period = 64;
    std::vector<MidiHit> hits;
    std::vector<float> l = std::vector<float>(4096), r = std::vector<float>(4096);

    std::int64_t frame() const { return static_cast<std::int64_t>(e.blocksRendered()) * engine::kBlockFrames; }
    void runPeriod()
    {
        // One period, rendered one 64-frame call at a time so every block's router events can be read, but the
        // engine's audio clock still sees the device period (beginPeriod is called per process() call: keep the
        // period at 64 for exact hits, or use runDevicePeriod()).
        float* out[2] = {l.data(), r.data()};
        for (int done = 0; done < period; done += engine::kBlockFrames)
        {
            e.process(out, 2, engine::kBlockFrames);
            collect();
        }
    }
    void runDevicePeriod(std::int64_t hostTime = 0)
    {
        float* out[12] = {l.data(), r.data()};
        e.process(engine::AudioIO {out, 1, nullptr, 0, period, static_cast<std::uint64_t>(hostTime)});
    }
    void collect()
    {
        const std::int64_t start = frame() - engine::kBlockFrames;
        for (const auto& ev : e.triggerRouter().blockEvents())
            if (ev.source == engine::trigger::TriggerSource::Midi)
                hits.push_back({start + ev.frame, ev.track, ev.semitones, ev.velocity, ev.kind});
    }
    void blocks(int n)
    {
        for (int i = 0; i < n; ++i)
            runPeriod();
    }
    void param(std::string_view name, std::uint16_t scope, float value)
    {
        e.bridge().post(engine::ParamChange {id(name), scope, value, 0});
        runPeriod();
    }
};

// A silent mono sample of `frames` frames, installed on `track` of scene 0 by the next block. The buffer must outlive
// the engine (declare it before the engine).
using Sample = std::unique_ptr<core::audio::SampleBuffer, core::audio::SampleBufferDeleter>;
inline Sample installSample(engine::Engine& e, int track, std::int64_t frames, double rate = 44100.0)
{
    Sample b(core::audio::allocateSampleBuffer(frames, 1, rate));
    core::audio::retain(b.get()); // the engine's reference (released when it is replaced or the engine goes)
    e.bridge().post(engine::ObjectSwap{{engine::SwapSlot::TrackSample, 0, static_cast<std::uint8_t>(track)}, b.get()});
    return b;
}

// Everything the engine sent to the hub's out queue so far (the test plays the sender).
inline std::vector<core::midi::MidiOutEvent> drainOut(engine::Engine& e)
{
    std::vector<core::midi::MidiOutEvent> v;
    core::midi::MidiOutEvent o;
    while (e.midi().hub().popOut(o))
        v.push_back(o);
    return v;
}
} // namespace miditest
