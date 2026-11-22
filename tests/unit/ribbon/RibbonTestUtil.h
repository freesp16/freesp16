// Test doubles for the ribbon tests: a ModMatrix stand-in with the set / clear signatures RibbonEngine::publish uses
// (the tests do not link ModMatrix.cpp: the real class is only compile-checked, test_ribbon_engine.cpp),
// a repeat sink, and block contexts at a tempo.
#pragma once

#include "engine/ribbon/RibbonEngine.h"

#include "core/params/ParamRegistry.h"

#include <map>
#include <tuple>
#include <vector>

namespace ribbontest
{
using engine::modulation::ModKind;
using engine::modulation::ModSource;

inline core::ParamId id(std::string_view name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

struct FakeMatrix
{
    struct Entry
    {
        ModKind kind;
        float value;
    };
    std::map<std::tuple<int, std::uint16_t, int>, Entry> live; // (track, dest, source)
    int sets = 0, clears = 0;
    int capacity = 1000; // set() refuses beyond this many live entries
    bool set(int track, core::ParamId dest, ModSource src, ModKind kind, float value) noexcept
    {
        const auto key = std::make_tuple(track, dest.value, static_cast<int>(src));
        if (! live.count(key) && static_cast<int>(live.size()) >= capacity)
            return false;
        live[key] = {kind, value};
        ++sets;
        return true;
    }
    void clear(int track, core::ParamId dest, ModSource src) noexcept
    {
        live.erase(std::make_tuple(track, dest.value, static_cast<int>(src)));
        ++clears;
    }
    const Entry* at(int track, core::ParamId dest, ModSource src = ModSource::Ribbon) const
    {
        const auto it = live.find(std::make_tuple(track, dest.value, static_cast<int>(src)));
        return it == live.end() ? nullptr : &it->second;
    }
    int count(ModSource src = ModSource::Ribbon) const
    {
        int n = 0;
        for (const auto& [k, v] : live)
            n += std::get<2>(k) == static_cast<int>(src) ? 1 : 0;
        return n;
    }
};

struct Hits
{
    std::vector<engine::trigger::TriggerEvent> events;
    bool push(const engine::trigger::TriggerEvent& e) noexcept
    {
        events.push_back(e);
        return true;
    }
};

inline engine::ribbon::RibbonEngine makeRibbon(double rate = 44100.0)
{
    engine::ribbon::RibbonEngine r;
    r.bindParams([](std::string_view s) { return id(s); });
    r.prepare(rate);
    return r;
}

// A pad event as the router applies it (source Pad).
inline engine::trigger::TriggerEvent padEvent(int track, bool press, int velocity = 100, int frame = 0)
{
    engine::trigger::TriggerEvent e {};
    e.track = static_cast<std::uint8_t>(track);
    e.velocity = static_cast<std::uint8_t>(press ? velocity : 0);
    e.kind = press ? engine::trigger::TriggerKind::Full : engine::trigger::TriggerKind::Release;
    e.source = engine::trigger::TriggerSource::Pad;
    e.frame = static_cast<std::uint8_t>(frame);
    return e;
}

inline void press(engine::ribbon::RibbonEngine& r, int track, bool down, int velocity = 100)
{
    const auto e = padEvent(track, down, velocity);
    r.observe(std::span<const engine::trigger::TriggerEvent>(&e, 1), {});
}

inline void idle(engine::ribbon::RibbonEngine& r) { r.observe({}, {}); }

// Block n of a transport running from PPQ 0 at `bpm` (stopped: ppq stays put).
inline engine::BlockContext block(long n, double bpm, bool playing, double rate = 44100.0)
{
    engine::BlockContext c {};
    c.sampleRate = rate;
    c.frames = engine::kBlockFrames;
    c.bpm = bpm;
    c.playing = playing;
    const double ppf = bpm / (60.0 * rate);
    c.ppqStart = playing ? static_cast<double>(n) * engine::kBlockFrames * ppf : 0.0;
    c.ppqEnd = playing ? static_cast<double>(n + 1) * engine::kBlockFrames * ppf : 0.0;
    return c;
}
} // namespace ribbontest
