// Step 6 of the engine block as engine::Engine holds it (stages/Stages.h aliases it as stages::SequencerStage): the
// Sequencer plus its set-up — the SwapSlot::Pattern objects it plays (mirrored from the engine's swap table once per
// block, two pointer loads), the EngineEvent queue, the router's LiveHitSink, and the `seq.*` parameters it applies
// (core/params/catalogue/sequencer.cpp; ParamChange scope as EngineApi.h):
//
//   seq.swing              Project   0–50 %            → setSwing(v / 100)
//   seq.recordQuantize     Project   enum 0–5          → setRecordQuantize
//   seq.padStartsSequence  Project   0 / 1             → setPadStartsSequence
//   seq.offset             Track     −24…+24 (1/24)    → setTrackOffset(scene, track)
//   seq.record             internal  0 / 1             → setRecordArmed (REC lit, SEQ-041)
//   seq.erase              internal  0 / 1, scope = track → setErase (SEQ-046)
//
// applyFxLocks (after the router applied the block's events, before the FX bank's beginBlock): at every sequencer
// trigger the track's FX1 / FX2 slots receive all six parameters — the step's FX lock (slots 23–28 / 29–34) or the
// stored value — so unlocked FX parameters snap back (SEQ-026, FX-079), from the block start. The model does not
// record the effect type a lock was made for yet (SEQ-025 / FX-080, P2): a lock applies to the type in the slot.
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/bridge/SwapTable.h"
#include "engine/feeds/Feeds.h"
#include "engine/fx/FxSlotState.h"
#include "engine/sequencer/PatternBuilder.h"
#include "engine/sequencer/Sequencer.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/trigger/TriggerRouter.h"

#include <functional>
#include <string_view>

namespace engine::sequencer
{
class SequencerStage
{
public:
    Sequencer seq;

    // ---- message thread (engine set-up) ----
    void attach(const bridge::SwapTable& swaps, feeds::EngineFeeds& feeds, trigger::TriggerRouter& router) noexcept
    {
        swaps_ = &swaps;
        seq.setEventQueue(&feeds);
        router.setLiveHitSink(&seq);
    }
    // Resolves the ids and binds them to ParamTarget::Sequencer (EngineBridge::post refuses unbound ids).
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                    bridge::ParamBindings& bindings) noexcept
    {
        if (! resolve)
            return;
        swing_ = resolve("seq.swing");
        quantize_ = resolve("seq.recordQuantize");
        padStarts_ = resolve("seq.padStartsSequence");
        offset_ = resolve("seq.offset");
        record_ = resolve("seq.record");
        erase_ = resolve("seq.erase");
        for (const auto id : {swing_, quantize_, padStarts_, offset_, record_, erase_})
            bindings.bind(id, bridge::ParamTarget::Sequencer);
    }
    void prepare(double sampleRate) noexcept { seq.prepare(sampleRate); }

    // ---- audio thread ----
    // A ParamChange for the sequencer: applied, true; anything else: false (the engine routes it on).
    bool onParam(const ParamChange& c) noexcept
    {
        const auto id = c.param;
        if (! id.valid())
            return false;
        const float v = c.value;
        if (id == swing_)
            seq.setSwing(v * 0.01f);
        else if (id == quantize_)
            seq.setRecordQuantize(core::sequencer::recordQuantizeOf(static_cast<int>(v + 0.5f)));
        else if (id == padStarts_)
            seq.setPadStartsSequence(v >= 0.5f);
        else if (id == offset_)
        {
            if (c.scope <= 0xFF)
                seq.setTrackOffset(c.scope >> 4, c.scope & 15,
                                   static_cast<std::int8_t>(v < 0.0f ? v - 0.5f : v + 0.5f));
        }
        else if (id == record_)
            seq.setRecordArmed(v >= 0.5f);
        else if (id == erase_)
            seq.setErase(c.scope & 15, v >= 0.5f);
        else
            return false;
        return true;
    }

    void process(BlockContext& ctx, stages::BlockBuffers& b, trigger::TriggerRouter& router) noexcept
    {
        if (swaps_ != nullptr)
        {
            mirror(b.timing.scene, b.timing.pattern);
            mirror(lastScene_, lastPattern_); // the pattern before a switch boundary (Sequencer: old before, new after)
        }
        lastScene_ = b.timing.scene;
        lastPattern_ = b.timing.pattern;
        seq.collect(ctx, b.timing, router);
    }

    // Bank: FxSlotBank (applyStepLocks, slot(s).pending()); a template so the stage does not depend on the bank's header.
    template <class Bank>
    void applyFxLocks(const trigger::TriggerRouter& router, Bank& bank) noexcept
    {
        for (const auto& e : router.blockEvents())
        {
            if (e.source != trigger::TriggerSource::Sequencer)
                continue;
            fx::FxStepLocks l;
            for (int u = 0; u < 2; ++u)
            {
                l.type[u] = static_cast<std::int8_t>(bank.slot(fx::insertSlot(e.track, u)).pending());
                if (e.locks == nullptr)
                    continue;
                for (int k = 0; k < 6; ++k)
                    if (e.locks->get(kFxLockSlot + 6 * u + k, l.value[u][k]))
                        l.mask[u] = static_cast<std::uint8_t>(l.mask[u] | (1u << k));
            }
            bank.applyStepLocks(e.track, l);
        }
    }

private:
    static constexpr int kFxLockSlot = 23; // FX1 k0 (FX2 k0 = 29; sequencer.md "Lockable parameters")
    void mirror(int scene, int pattern) noexcept
    {
        const SwapSlot slot {SwapSlot::Pattern, static_cast<std::uint8_t>(scene & 15),
                             static_cast<std::uint8_t>(pattern & 15)};
        seq.install(scene, pattern, static_cast<const PatternData*>(swaps_->get(slot)));
    }

    const bridge::SwapTable* swaps_ = nullptr;
    core::ParamId swing_ {}, quantize_ {}, padStarts_ {}, offset_ {}, record_ {}, erase_ {};
    std::uint8_t lastScene_ = 0, lastPattern_ = 0;
};
} // namespace engine::sequencer
