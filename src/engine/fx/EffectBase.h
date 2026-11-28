// The effect contract. Behaviour = 02-features/effects/effect-shell.md
// (FX-020…057) and research docs/effects/: one instance per effect type per slot, built and prepared at start-up
// (threading.md: a type change only switches the active index and resets that effect, FX-032). The FxShell
// (engine/fx/FxShell*) wraps every effect with gate modes, input fade, the 10 Hz wet high-pass, the mix
// law and bypass ramps; an effect renders only its wet signal.
//
// Real-time and latency contract: process() is noexcept, never allocates or locks, runs within its measured budget
// (bench_fx), handles exactly 64 frames in place, and reports latency honestly (no compensation, parity).
#pragma once

#include "engine/fx/EffectDescriptor.h"
#include "engine/fx/EffectRegistry.h"

#include <array>
#include <cstdint>

namespace engine::fx
{
struct ProcessContext {
    double sampleRate; double bpm; bool transportRunning;
    std::uint32_t triggerMask;                       // tracks triggered in this block (Ducker feed)
    std::array<std::int16_t, 16> triggerOffset;      // in-block sample offset per triggered track (FX-1242), -1 = none
    std::uint16_t midiTrackMask;                     // tracks in MIDI mode (FX-1263)
    double ppqStart = 0;
};

class EffectBase {                                   // one instance per effect type per slot, built at start-up
public:
    virtual ~EffectBase() = default;
    virtual void prepare(double sampleRate, int maxBlock) = 0;   // allocate everything here (delay lines at max)
    virtual void reset() noexcept = 0;                            // FX-032: activation / type change
    virtual void setParam(int index, float value01) noexcept = 0; // 0..5, stored step fraction (FX-060)
    virtual void setOption(int which, int value) noexcept { (void) which; (void) value; } // type1/type2 enums, sync, lookAhead
    // Exactly 64 frames, planar stereo, in place, wet only (the shell does dry/wet, gate, 10 Hz HPF).
    virtual void process(float* left, float* right, const ProcessContext&) noexcept = 0;
    virtual bool available() const noexcept { return true; }
    virtual int latencySamples() const noexcept { return 0; }     // non-zero only with look-ahead (2 ms)
    virtual void retrigger() noexcept {}                          // FX-089/090: transport start, pad/step retrigger
    virtual void setQuirkProfile(std::uint32_t mask) noexcept { (void) mask; } // FX-052…055: reference quirks on/off (parity mode)
    virtual float telemetry(int which) const noexcept { (void) which; return 0; } // UID-834: gain reduction, duck envelope (UI reads
                                                                    // via the meter feed, never directly)
};
// setOption codes: 0…99 = the effect's own type1/type2/sync/lookAhead options; reserved ≥ 100:
enum ReservedOption : int { kQuirkMask = 100, kDuckTriggerOffset = 101, kClearState = 102, kRetrigger = 103 };

class FxShell; // EffectBase wrapper: gate modes, input fade, 10 Hz wet HPF, mix, bypass ramps (FX-020…057)

// Helper for EffectDescriptor::create: &make<MyEffect>.
template <typename T>
std::unique_ptr<EffectBase> make()
{
    return std::make_unique<T>(); // rt-ok: start-up, message thread (one instance per type per slot)
}
} // namespace engine::fx
