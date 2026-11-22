#include "services/library/PreviewService.h"

#include "core/audio/SampleBufferStorage.h"

#include <algorithm>
#include <optional>

namespace services::library
{
PreviewService::PreviewService(SamplePool& pool, Workers& workers, engine::EngineBridge& bridge, Clock clock)
    : pool_(pool), workers_(workers), bridge_(bridge), clock_(std::move(clock))
{
}

PreviewService::~PreviewService()
{
    *alive_ = false;
    if (inFlight_)
        inFlight_->store(true);
    // Buffers the engine still holds are released by its Preview destroy function later; only free spent ones.
    for (auto* b : owned_)
        if (b->refs.load(std::memory_order_acquire) == 0)
            core::audio::freeSampleBuffer(b);
}

void PreviewService::focus(const juce::File& file)
{
    if (file == focused_ && pending_)
        return;
    focused_ = file;
    if (inFlight_)
        inFlight_->store(true); // superseded
    inFlight_.reset();
    pending_ = enabled_ && file.existsAsFile();
    focusAt_ = now();
    if (! pending_ && playing_ != juce::File())
        stop(); // focus on a folder or a missing file: moving away stops the old preview
}

void PreviewService::setEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (! on)
    {
        pending_ = false;
        stop();
        return;
    }
    if (focused_.existsAsFile())
    {
        pending_ = false;
        start(focused_); // LIB-011: switching on plays the focused file once, at once
    }
}

void PreviewService::stop()
{
    if (inFlight_)
        inFlight_->store(true);
    inFlight_.reset();
    pending_ = false;
    playing_ = juce::File();
    bridge_.post(engine::ObjectSwap{engine::SwapSlot{engine::SwapSlot::Preview, 0, 0}, nullptr});
}

void PreviewService::tick()
{
    if (pending_ && now() - focusAt_ >= kDebounceMs)
    {
        pending_ = false;
        start(focused_);
    }
    std::erase_if(owned_,
                  [](const core::audio::SampleBuffer* b)
                  {
                      if (b->refs.load(std::memory_order_acquire) != 0)
                          return false;
                      core::audio::freeSampleBuffer(b);
                      return true;
                  });
}

void PreviewService::publish(const juce::File& file, SamplePool::Handle buffer)
{
    core::audio::retain(buffer); // the engine's reference (its Preview destroy function releases it)
    if (! bridge_.post(engine::ObjectSwap{engine::SwapSlot{engine::SwapSlot::Preview, 0, 0}, buffer}))
    {
        core::audio::release(buffer);
        return;
    }
    playing_ = file;
}

void PreviewService::start(const juce::File& file)
{
    if (SamplePool::Handle resident = pool_.find(file))
    {
        publish(file, resident);
        return;
    }
    auto cancel = makeCancelToken();
    inFlight_ = cancel;
    struct Out
    {
        std::optional<core::Result<DecodedAudio>> decoded;
    };
    auto out = std::make_shared<Out>();
    const double rate = pool_.engineRate();
    std::weak_ptr<bool> alive = alive_;
    workers_.run(
        [file, rate, cancel, out]
        {
            if (cancel->load())
                return;
            DecodeReport report;
            DecodeOptions options;
            options.maxSeconds = kMaxSeconds;
            options.cancel = cancel.get();
            options.limitBytes = 256ll << 20; // a preview never takes more than this
            out->decoded.emplace(Decoder::decode(file, rate, report, options));
        },
        [this, alive, file, cancel, out]
        {
            if (alive.expired() || ! out->decoded)
                return;
            auto& r = *out->decoded;
            if (cancel->load() || inFlight_ != cancel)
                return; // superseded: the buffer (refs 0, never published) is freed with `out`
            inFlight_.reset();
            if (! r)
            {
                if (onError)
                    onError(r.error());
                return;
            }
            const core::audio::SampleBuffer* buffer = r.value().buffer.release();
            owned_.push_back(buffer);
            publish(file, buffer);
        });
}
} // namespace services::library
