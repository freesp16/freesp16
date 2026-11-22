// Helpers for the mixer unit tests: a heap Mixer + BlockBuffers (a BlockBuffers is ~10 KB: never on a test's stack),
// deterministic signals, and a fake of engine::trigger::LockSet (its shape only, no dependency on the trigger module).
#pragma once

#include "engine/mixer/Mixer.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace mixertest
{
constexpr int kN = engine::kBlockFrames;

struct Rig
{
    std::unique_ptr<engine::mixer::Mixer> mixer = std::make_unique<engine::mixer::Mixer>();
    std::unique_ptr<engine::stages::BlockBuffers> b = std::make_unique<engine::stages::BlockBuffers>();
    engine::mixer::Hooks hooks {};
    engine::BlockContext ctx {};

    engine::mixer::Mixer& m() { return *mixer; }
    // One block; every track in `active` gets `value(track, frame)` on both channels.
    template <class Fn> void block(std::uint32_t active, Fn value)
    {
        b->activeTracks = active;
        for (int t = 0; t < engine::kTracks; ++t)
            for (int i = 0; i < kN; ++i)
                b->track[static_cast<std::size_t>(t)].l[i] = b->track[static_cast<std::size_t>(t)].r[i] = value(t, i);
        mixer->process(ctx, *b, hooks);
        if (! mixer->sendBusUsed())
            b->sendBus.clear(); // an untapped send bus is left stale by the mixer: zero it for the checks
    }
    void dc(std::uint32_t active, float v = 1.0f)
    {
        block(active, [v](int, int) { return v; });
    }
};

// xorshift noise in [-a, a)
struct Noise
{
    std::uint32_t s = 0x12345678u;
    float next(float a = 1.0f)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return a * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
    }
};

struct FakeLockSet // engine::trigger::LockSet's shape
{
    std::uint64_t mask = 0;
    const float* values = nullptr;
    const std::uint8_t* change = nullptr;
    bool get(int slot, float& v) const noexcept
    {
        if (! (mask >> slot & 1u))
            return false;
        v = values[slot];
        return true;
    }
};

inline engine::mixer::StripParams strip(int volume = 100, int pan = 0, int send = 0, bool mute = false, bool solo = false)
{
    engine::mixer::StripParams p;
    p.volume = static_cast<std::uint8_t>(volume);
    p.pan = static_cast<std::int8_t>(pan);
    p.send = static_cast<std::uint8_t>(send);
    p.mute = mute;
    p.solo = solo;
    return p;
}
} // namespace mixertest
