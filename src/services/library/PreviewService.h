// The browser preview's feed (LIB-011, UID-677): when the focus settles on a file
// (120 ms debounce, so fast scrolling stays silent) the first 30 s are decoded — or the pooled buffer is used when
// the file is resident — and handed to the engine as ObjectSwap{SwapSlot{Preview, 0, 0}, buffer}; installing it
// starts one playback (the engine's PreviewVoice). A new focus cancels an in-flight decode; stop() and PREVIEW off
// post a null swap. Switching PREVIEW on plays the focused file once.
//
// Preview buffers that are not pooled belong to this service: it retains one reference for the engine with each swap
// and frees a buffer once its count is back to 0 (tick(), never on the audio thread). Budget: playback posted ≤ 80 ms
// after the debounce for a file ≤ 5 s (bench_decode `decode_preview_5s`). Message thread only; the owner calls tick()
// once per frame.
#pragma once

#include "engine/EngineApi.h"
#include "services/library/Decoder.h"
#include "services/library/SamplePool.h"
#include "services/library/Workers.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

namespace services::library
{
class PreviewService
{
public:
    static constexpr double kDebounceMs = 120;
    static constexpr double kMaxSeconds = 30;
    using Clock = std::function<double()>; // milliseconds, monotonic

    PreviewService(SamplePool&, Workers&, engine::EngineBridge&, Clock clock = {});
    ~PreviewService();
    PreviewService(const PreviewService&) = delete;
    PreviewService& operator=(const PreviewService&) = delete;

    void focus(const juce::File&); // the browser's focus moved to this file (a folder: juce::File())
    void setEnabled(bool);         // browse.preview (default ON)
    bool enabled() const noexcept { return enabled_; }
    void stop(); // silence now (leaving BROWSE, loading the file)
    void tick(); // frame loop: debounce, frees spent buffers

    const juce::File& focused() const noexcept { return focused_; }
    const juce::File& playing() const noexcept { return playing_; } // last file handed to the engine
    std::function<void(const core::Error&)> onError;                // a file that cannot be previewed
    std::size_t ownedBuffers() const noexcept { return owned_.size(); }

private:
    void start(const juce::File&);
    void publish(const juce::File&, SamplePool::Handle);
    double now() const { return clock_ ? clock_() : juce::Time::getMillisecondCounterHiRes(); }

    SamplePool& pool_;
    Workers& workers_;
    engine::EngineBridge& bridge_;
    Clock clock_;
    bool enabled_ = true;
    juce::File focused_, playing_;
    double focusAt_ = 0;
    bool pending_ = false;
    CancelToken inFlight_;
    std::vector<const core::audio::SampleBuffer*> owned_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::library
