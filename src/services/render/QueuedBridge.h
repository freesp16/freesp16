// An EngineBridge that paces large bursts of posts (the sending thread only: the composition root's message thread, or
// the offline render's worker, services/render/OfflineEngine.h). A project open posts
// every scene's values (resident scenes: 16 × the track table, the FX slots, 256 patterns): ≈ 15,000 messages,
// more than the engine's 4,096-entry FIFO holds. At most `budget` messages go through per flush() period (one UI
// frame live; one block offline); the rest wait here, in order, and flush() hands them on.
//
// Refusals: within the budget the FIFO has room (the engine drains up to 512 messages per block, ≥ 10 blocks per UI
// frame), so a refusal means the engine does not take that message at all (an unbound parameter, a swap kind
// without a consumer) — as from the engine's own bridge: post() returns false at once, the sender keeps ownership.
// A queued message refused later is dropped here; a queued ObjectSwap belongs to the queue until it is posted and is
// given back when it is dropped or still queued at destruction (TrackSample/Preview: the buffer reference; Pattern:
// the pattern compiler's destroy; SliceMap: the slice table; LiveTake: the capture hub's reference).
#pragma once

#include "core/audio/PluginNode.h"
#include "core/audio/SampleBuffer.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "core/util/chain/ChainPlan.h"
#include "core/util/sampling/CaptureHub.h"
#include "core/util/sequencer/PatternImage.h"
#include "core/util/slice/SliceTable.h"
#include "engine/EngineApi.h"

#include <deque>
#include <memory>
#include <variant>

namespace services::render
{
class QueuedBridge final : public engine::EngineBridge
{
public:
    explicit QueuedBridge(engine::EngineBridge& inner, int budget = 1024) : inner_(inner), budget_(budget) {}
    ~QueuedBridge() override
    {
        for (const auto& m : pending_)
            giveBack(m);
    }
    QueuedBridge(const QueuedBridge&) = delete;
    QueuedBridge& operator=(const QueuedBridge&) = delete;

    void stageTrackSwap(engine::TrackSwapBatch::Swap swap)
    {
        if (! trackSwap_)
            trackSwap_ = std::make_unique<engine::TrackSwapBatch>(&giveBackSwap);
        trackSwap_->swaps.push_back(swap);
    }

    void finishTrackSwaps()
    {
        if (! trackSwap_)
            return;
        const engine::ObjectSwap message{{engine::SwapSlot::TrackSwapBatch, 0, 0}, trackSwap_.release()};
        if (! post(message))
            pending_.push_back(message); // the FIFO can be full while the swap is being prepared
    }

    bool post(const engine::EngineMessage& m) override
    {
        if (trackSwap_)
        {
            if (const auto* param = std::get_if<engine::ParamChange>(&m))
                trackSwap_->edits.emplace_back(*param);
            else if (const auto* swap = std::get_if<engine::ObjectSwap>(&m))
            {
                trackSwap_->edits.emplace_back(*swap);
                ++trackSwap_->retireSlots;
            }
            else
                return false;
            return true;
        }
        if (pending_.empty() && sent_ < budget_)
        {
            ++sent_;
            return inner_.post(m);
        }
        pending_.push_back(m);
        return true;
    }
    // A new period: up to `budget` queued messages go on. True when nothing is left.
    bool flush()
    {
        sent_ = 0;
        while (! pending_.empty() && sent_ < budget_)
        {
            const engine::EngineMessage m = pending_.front();
            ++sent_;
            if (! inner_.post(m))
            {
                const auto* swap = std::get_if<engine::ObjectSwap>(&m);
                if (swap && swap->slot.kind == engine::SwapSlot::TrackSwapBatch)
                    break; // retry the whole swap in order when the audio thread has room
                giveBack(m);
            }
            pending_.pop_front();
        }
        return pending_.empty();
    }
    std::size_t pending() const noexcept { return pending_.size(); }

    void apply(const core::ChangeSet& c, const engine::model::Project& p) override { inner_.apply(c, p); }
    const engine::PlayheadFeed& playhead() const override { return inner_.playhead(); }
    const engine::MeterFeed& meters() const override { return inner_.meters(); }
    void retirePending() override { inner_.retirePending(); }
    bool popEvent(engine::EngineEvent& out) override { return inner_.popEvent(out); }
    const engine::fx::FxMeterFeed* fxMeters() const override { return inner_.fxMeters(); }

private:
    static void giveBack(const engine::EngineMessage& m)
    {
        if (const auto* swap = std::get_if<engine::ObjectSwap>(&m))
            giveBackSwap(*swap);
    }

    static void giveBackSwap(const engine::ObjectSwap& swap)
    {
        if (swap.object == nullptr)
            return;
        if (swap.slot.kind == engine::SwapSlot::TrackSwapBatch)
            delete static_cast<const engine::TrackSwapBatch*>(swap.object);
        else if (swap.slot.kind == engine::SwapSlot::TrackSample || swap.slot.kind == engine::SwapSlot::Preview)
            core::audio::release(static_cast<const core::audio::SampleBuffer*>(swap.object));
        else if (swap.slot.kind == engine::SwapSlot::TrackPlugin || engine::pluginFxIndex(swap.slot) >= 0)
            core::audio::releasePluginNode(swap.object);
        else if (swap.slot.kind == engine::SwapSlot::Pattern && core::sequencer::patternCompiler().destroy != nullptr)
            core::sequencer::patternCompiler().destroy(swap.object);
        else if (swap.slot.kind == engine::SwapSlot::ProjectConfig) // the pattern chain
            core::chain::destroyChainPlan(swap.object);
        else if (swap.slot.kind == engine::SwapSlot::SceneConfig) // the arrangement plan
            core::arrangement::destroyArrangementPlan(swap.object);
        else if (swap.slot.kind == engine::SwapSlot::SliceMap) // a slice table
            core::slice::destroySliceTable(swap.object);
        else if (swap.slot.kind == engine::SwapSlot::LiveTake) // the capture hub's engine reference
            core::sampling::releaseCaptureHub(swap.object);
    }

    engine::EngineBridge& inner_;
    int budget_, sent_ = 0;
    std::deque<engine::EngineMessage> pending_;
    std::unique_ptr<engine::TrackSwapBatch> trackSwap_;
};
} // namespace services::render
