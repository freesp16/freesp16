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
#include "core/util/sequencer/PatternImage.h"
#include "core/util/sampling/CaptureHub.h"
#include "core/util/slice/SliceTable.h"
#include "engine/EngineApi.h"

#include <deque>
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

    bool post(const engine::EngineMessage& m) override
    {
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
            pending_.pop_front();
            ++sent_;
            if (! inner_.post(m))
                giveBack(m);
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
        const auto* s = std::get_if<engine::ObjectSwap>(&m);
        if (s == nullptr || s->object == nullptr)
            return;
        if (s->slot.kind == engine::SwapSlot::TrackSample || s->slot.kind == engine::SwapSlot::Preview)
            core::audio::release(static_cast<const core::audio::SampleBuffer*>(s->object));
        else if (s->slot.kind == engine::SwapSlot::TrackPlugin || engine::pluginFxIndex(s->slot) >= 0)
            core::audio::releasePluginNode(s->object);
        else if (s->slot.kind == engine::SwapSlot::Pattern && core::sequencer::patternCompiler().destroy != nullptr)
            core::sequencer::patternCompiler().destroy(s->object);
        else if (s->slot.kind == engine::SwapSlot::ProjectConfig) // the pattern chain
            core::chain::destroyChainPlan(s->object);
        else if (s->slot.kind == engine::SwapSlot::SceneConfig) // the arrangement plan
            core::arrangement::destroyArrangementPlan(s->object);
        else if (s->slot.kind == engine::SwapSlot::SliceMap) // a slice table
            core::slice::destroySliceTable(s->object);
        else if (s->slot.kind == engine::SwapSlot::LiveTake) // the capture hub's engine reference
            core::sampling::releaseCaptureHub(s->object);
    }

    engine::EngineBridge& inner_;
    int budget_, sent_ = 0;
    std::deque<engine::EngineMessage> pending_;
};
} // namespace services::render
