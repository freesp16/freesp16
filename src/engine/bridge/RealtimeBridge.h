// The engine half of EngineBridge (threading.md "Channels between threads"):
//
//   message thread ── post() ──▶ SPSC (4096, kMessageQueueSize) ──▶ audio thread: pop() at block start (step 1)
//   message thread ◀── playhead()/meters()/popEvent() ── triple buffers + SPSC ◀── audio thread (EngineFeeds)
//   audio thread ── retire() ──▶ RetireQueue ──▶ message thread: retirePending() frees after >= 2 blocks
//
// post() validates on the message thread, so a message the engine cannot use is refused at once (false) and an
// ObjectSwap's ownership stays with the sender: an unbound ParamId, a swap slot kind no consumer accepts (SwapTable) or
// a full queue. apply() — the model diff — lives outside engine/ (it needs core/model/Project.h, which the engine may
// never include): here it is a no-op, and the services-side translator forwards to post().
//
// TriggerQueue is the TriggerSink of platform::ControlEnv: multi-producer (pad reader, MIDI readers), drained by
// the audio thread at block start.
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/bridge/SwapTable.h"
#include "engine/feeds/Feeds.h"

#include "core/util/MpscQueue.h"
#include "core/util/RetireQueue.h"
#include "core/util/SpscQueue.h"

#include <atomic>
#include <cstdint>

namespace engine::bridge
{
class TriggerQueue final : public TriggerSink
{
public:
    static constexpr std::size_t kCapacity = 256;
    bool push(const TriggerNow& t) noexcept override { return t.track < kTracks && queue_.push(t); } // any thread
    bool pop(TriggerNow& out) noexcept { return queue_.pop(out); }                                    // audio thread

private:
    core::MpscQueue<TriggerNow, kCapacity> queue_;
};

class RealtimeBridge final : public EngineBridge
{
public:
    static constexpr std::size_t kRetireCapacity = 1024;

    RealtimeBridge(const ParamBindings& params, const SwapTable& swaps, feeds::EngineFeeds& feeds) noexcept
        : params_(params), swaps_(swaps), feeds_(feeds)
    {
    }
    ~RealtimeBridge() override;

    // ---- EngineBridge (message thread) ----
    bool post(const EngineMessage&) override;
    void apply(const core::ChangeSet&, const model::Project&) override {} // the diff lives outside engine/
    const PlayheadFeed& playhead() const override { return feeds_.playhead(); }
    const MeterFeed& meters() const override { return feeds_.meterSnapshot(); }
    void retirePending() override { retire_.collect(blocks_.load(std::memory_order_acquire)); }
    bool popEvent(EngineEvent& out) override { return feeds_.popEvent(out); }
    const fx::FxMeterFeed* fxMeters() const override { return &feeds_.fxSnapshot(); }

    // Message thread (engine shutdown, the device stopped): frees every retired object regardless of age and every
    // ObjectSwap still queued (ownership passed to the engine when post() returned true).
    void drainAll() noexcept;
    std::size_t retiredPending() const noexcept { return retire_.pending(); } // message thread (tests)

    // ---- audio thread ----
    bool pop(EngineMessage& out) noexcept { return queue_.pop(out); }
    // The old object of a swap; false when the retire queue is full (the caller keeps it installed and retries).
    bool retire(const void* object, SwapSlot::Kind kind, std::uint64_t blockIndex) noexcept
    {
        return retire_.retire(object, swaps_.destroyer(kind), blockIndex);
    }
    bool canRetire(std::size_t count) noexcept { return retire_.canRetire(count); }
    void setBlocksRendered(std::uint64_t blocks) noexcept { blocks_.store(blocks, std::memory_order_release); }

private:
    const ParamBindings& params_;
    const SwapTable& swaps_;
    feeds::EngineFeeds& feeds_;
    core::SpscQueue<EngineMessage, kMessageQueueSize> queue_;
    core::RetireQueue<kRetireCapacity> retire_;
    std::atomic<std::uint64_t> blocks_ {0};
};
} // namespace engine::bridge
