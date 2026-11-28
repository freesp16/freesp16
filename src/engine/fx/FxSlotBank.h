// FxSlotBank — the 34 FX slots of one engine (spec 02-features/effects/README.md FX-002…012,
// effect-shell.md, parameters.md FX-056/076…094): 16 tracks × {FX1, FX2} inserts in series, the Send FX on the send
// bus (output = return, no dry path, FX-003) and the Master FX on the main bus. Every slot holds one instance of every
// type its kind lists (EffectRegistryOrder.h listFor), built and prepared on the message thread (FX-005).
//
// Parameter state per slot and type: stored[k] (the model's value) and, for the slot's current type, effective[k]
// (stored, or a step lock until the next trigger, FX-079…081), plus an LFO amount per k (FX-084:
// clamp(effective + amount, 0, 1)). Every write reaches the effect at the block start it is drained in (FX-056/076).
//
// ---------------------------------------------------------------------------------------------------------------------
// INTEGRATION (how engine/Engine.{h,cpp} and engine/stages/Stages.h drive the bank).
// The bank plugs into the mixer's FX seams (engine/mixer/Mixer.h `Hooks`): one BusInsert per track insert
// pair, the Send FX and the Master FX. The bank is a plain member of engine::Engine (build() + prepare() on the message
// thread before the device starts, with the ParamSpec ids of core::builtinParams() and core::fx::internalIndexTable).
// Per 64-frame block:
//
//   1. message drain (step 1): bind every ParamId with fxBank_.routes().find(id).kind != None to a ParamTarget::Fx at
//      set-up, and in Engine::apply:   case ParamTarget::Fx: fxBank_.onParamChange(*c); break;
//      scene switch: fxBank_.applyScene(state) with the FxSlotState the bridge prepared.
//   2. after TransportStage (step 3), before the tracks:
//          fxBank_.beginBlock(ctx, midiTrackMask);   // tempo/trigger context, FX-089 restart, FX-002 MIDI disable
//          b.activeTracks |= fxBank_.claimTailTracks(b);  // silent tracks whose inserts still ring: cleared buffer
//          fxBank_.fillHooks(hooks_);                // idle slots → null hooks (≈ 0 cycles; Send null = silent return)
//          mixer_.process(ctx, b, hooks_);           // the mixer calls insert[t] / sendFx / masterFx once per block
//   3. step 14 (feeds): fxBank_.publish(feed) once per period.
//   Other callers through the same block-start drain: applyStepLocks(track, locks) at each trigger (the sequencer's
//   lock applier), setModulation(...) once per block, silenceAndClear() on the second STOP.
//   Without the mixer, the same work is processInsert / processSend / processMaster below.
// ---------------------------------------------------------------------------------------------------------------------
//
// Budget (measured by tests/bench/bench_fx*.cpp): idle slot ≤ 25 cycles (all 34 idle ≤ 1 k); a running slot,
// shell only, ≤ 450; with the wet HPF ≤ 900; framework worst case (34 running, HPF) ≤ 31 k cycles per block.
#pragma once

#include "engine/fx/FxMode.h"
#include "engine/fx/FxProcessor.h"
#include "engine/fx/FxSlotRoutes.h"
#include "engine/fx/FxSlotState.h"
#include "engine/mixer/Mixer.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace engine::fx::effects
{
class Plugin;
}
namespace core::audio
{
class PluginAudioNode;
}

namespace engine::fx
{
class EffectRegistry;

class FxSlotBank
{
public:
    FxSlotBank();
    ~FxSlotBank();
    FxSlotBank(const FxSlotBank&) = delete;
    FxSlotBank& operator=(const FxSlotBank&) = delete;

    // Message thread. Builds every slot from the registry (types in listFor order with `enabledFlags`) and the route
    // table from the ParamSpec ids in registry order (index = ParamId.value). internalIndex (optional, same indexing):
    // the internal index k of each FX knob ParamSpec, −1 elsewhere (core::fx::internalIndexTable).
    void build(const EffectRegistry& registry, std::span<const std::string_view> paramIdsInRegistryOrder,
               std::span<const std::string_view> enabledFlags = {}, std::span<const std::int8_t> internalIndex = {});
    bool prepare(double sampleRate, int maxBlock); // message thread: allocates every effect's buffers

    // ---- control (audio thread, block start) ----
    void onParamChange(const ParamChange&) noexcept;             // ParamId → (slot, type, k) via routes()
    void applyScene(const FxSlotState& scene) noexcept;          // FX-004/045, at the switch block
    void applyStepLocks(int track, const FxStepLocks&) noexcept; // FX-079: locked or stored value, every k
    void setModulation(int track, int unit, int type, int k, float amount) noexcept; // FX-084
    void setFxMode(std::uint8_t both0send1master2) noexcept;                         // FX-008/009
    void setQuirkProfile(std::uint32_t mask) noexcept;                               // FX-052/053
    void setRestartLfosOnPlay(bool on) noexcept { restartLfos_ = on; }               // preference, default on (FX-089)
    void setActiveScene(std::uint8_t scene) noexcept { scene_ = scene; } // ParamChanges of other scenes are ignored
    void setMeteredSlot(int slot) noexcept;                              // −1 = none
    void onTransportStart() noexcept;                                    // FX-089: retrigger the LFO effects
    void silenceAndClear() noexcept;                                     // FX-036: second STOP

    // ---- the stage API (audio thread) ----
    ProcessContext beginBlock(const BlockContext& ctx, std::uint16_t midiTrackMask) noexcept;
    bool insertIdle(int track) const noexcept
    {
        return slots_[static_cast<std::size_t>(2 * track)]->idle() &&
               slots_[static_cast<std::size_t>(2 * track + 1)]->idle();
    }
    bool sendIdle() const noexcept { return slots_[kSendSlot]->idle(); }
    void swapInserts(int a, int b) noexcept;
    void setPlugin(int slot, core::audio::PluginAudioNode* node) noexcept;
    void processInsert(int track, float* l, float* r, const ProcessContext& ctx) noexcept
    {
        auto& a = *slots_[static_cast<std::size_t>(2 * track)];
        auto& b = *slots_[static_cast<std::size_t>(2 * track + 1)];
        if (! a.idle())
            a.process(l, r, ctx);
        if (! b.idle())
            b.process(l, r, ctx);
    }
    bool processSend(float* l, float* r, const ProcessContext& ctx) noexcept
    {
        auto& s = *slots_[kSendSlot];
        return s.idle() ? false : s.process(l, r, ctx);
    }
    void processMaster(float* l, float* r, const ProcessContext& ctx) noexcept
    {
        auto& m = *slots_[kMasterSlot];
        if (! m.idle())
            m.process(l, r, ctx);
    }
    void publish(FxMeterFeed&) const noexcept;

    // ---- the mixer seams (engine/mixer/Mixer.h Hooks) ----
    // The context of the block (set by beginBlock), used by the hooks.
    const ProcessContext& context() const noexcept { return ctx_; }
    // Idle slots get null hooks (Send null = Send FX Off = silent return, which is what an idle Send produces).
    void fillHooks(mixer::Hooks& h) noexcept;
    // Tracks not in `b.activeTracks` whose inserts are not idle (delay/reverb tails): their buffers are cleared so the
    // insert runs from silence; the caller ORs the result into b.activeTracks.
    std::uint32_t claimTailTracks(stages::BlockBuffers& b) noexcept;
    bool masterIdle() const noexcept { return slots_[kMasterSlot]->idle(); }

    // ---- inspection (tests, UI through the feed, the bridge at set-up) ----
    FxProcessor& slot(int s) noexcept { return *slots_[static_cast<std::size_t>(s)]; }
    const FxProcessor& slot(int s) const noexcept { return *slots_[static_cast<std::size_t>(s)]; }
    const FxSlotRoutes& routes() const noexcept { return routes_; }
    std::span<const EffectDescriptor* const> insertTypes() const noexcept { return insertTypes_; }
    std::span<const EffectDescriptor* const> sendTypes() const noexcept { return sendTypes_; }
    std::span<const EffectDescriptor* const> masterTypes() const noexcept { return masterTypes_; }
    float stored(int s, int type, int k) const noexcept;
    float effective(int s, int k) const noexcept { return state_[static_cast<std::size_t>(s)].effective[k]; }
    std::uint8_t lockMask(int s) const noexcept { return state_[static_cast<std::size_t>(s)].lockMask; }
    FxMode fxMode() const noexcept { return mode_; }

private:
    struct SlotParams
    {
        std::vector<float> stored; // [type][6]
        float effective[6]{};      // of the pending type
        float amount[6]{};         // LFO
        std::uint8_t lockMask = 0, modMask = 0;
        std::int8_t modType = -1;
    };
    int slotOf(const FxRoute& r, std::uint16_t scope, bool& ok) const noexcept;
    void push(int s, int type, int k) noexcept; // effective (+ modulation) of k → the effect
    void retype(int s, int type) noexcept;      // a type change: effective := stored of the new type, locks off

    class Hook final : public mixer::BusInsert
    {
    public:
        void process(float* l, float* r) noexcept override;
        FxSlotBank* bank = nullptr;
        int which = 0; // track 0…15, kSendSlot, kMasterSlot
    };

    std::array<std::unique_ptr<FxProcessor>, kFxSlots> slots_{};
    std::array<Hook, kTracks + 2> hooks_{};
    std::array<effects::Plugin*, kFxSlots> plugins_{};
    ProcessContext ctx_{44100.0, 120.0, false, 0u, {}, 0};
    std::array<SlotParams, kFxSlots> state_{};
    std::vector<const EffectDescriptor*> insertTypes_, sendTypes_, masterTypes_;
    FxSlotRoutes routes_{};
    FxMode mode_ = FxMode::Both;
    std::uint8_t scene_ = 0;
    std::uint16_t midiMask_ = 0;
    std::uint32_t triggerFlash_ = 0;
    std::uint64_t blockIndex_ = 0;
    int metered_ = -1;
    bool restartLfos_ = true;
};
} // namespace engine::fx
