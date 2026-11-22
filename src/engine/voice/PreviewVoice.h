// The BROWSE preview voice (LIB-011): plays the SampleBuffer installed in SwapSlot::Preview
// once, from its first frame, added into BlockContext's aux pair (BlockBuffers::aux, post-master). A different
// buffer (or none) fades the current preview out over 2 ms and starts the new one; installing the same pointer again
// changes nothing, so a finished preview stays silent until the next one is posted. Gain changes ramp over one
// block. Real-time: noexcept, no allocation.
#pragma once

#include "engine/EngineApi.h"
#include "engine/voice/VoiceHead.h"

namespace engine::voice
{
class PreviewVoice
{
public:
    PreviewVoice() = default;
    ~PreviewVoice();
    PreviewVoice(const PreviewVoice&) = delete;
    PreviewVoice& operator= (const PreviewVoice&) = delete;

    void prepare(double sampleRate); // message thread

    // ---- audio thread, block start ----
    void install(const SampleBuffer*) noexcept;
    void setGain(float) noexcept; // linear, ramped over the next block
    void render(float* auxL, float* auxR) noexcept; // adds 64 frames
    bool active() const noexcept { return head_.active || fading_.active; }
    const SampleBuffer* installed() const noexcept { return installed_; }

private:
    double engineRate_ = 44100.0;
    int fadeFrames_ = 88;
    const SampleBuffer* installed_ = nullptr; // identity only (the heads hold the references)
    Head head_, fading_;
    float gain_ = 1.0f, target_ = 1.0f;
    alignas(16) float scratch_[2][kBlockFrames] {};
    alignas(16) float seam_[2][kBlockFrames] {};
};
} // namespace engine::voice
