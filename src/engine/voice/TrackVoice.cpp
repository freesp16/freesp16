#include "engine/voice/TrackVoice.h"

#include <cmath>
#include <cstring>

namespace engine::voice
{
namespace
{
constexpr float kMaxTotalCents = 4800.0f; // note pitch ±2400 + modulation, bounded so the step stays sane

int msToFrames(float ms, double rate) noexcept
{
    const long n = std::lround(static_cast<double>(ms) * 0.001 * rate);
    return n < 1 ? 1 : static_cast<int>(n);
}

std::uint8_t clampFrame(int frame) noexcept
{
    return static_cast<std::uint8_t>(frame < 0 ? 0 : (frame > kBlockFrames - 1 ? kBlockFrames - 1 : frame));
}

void zero(float* const out[2], int n) noexcept
{
    std::memset(out[0], 0, static_cast<std::size_t>(n) * sizeof(float));
    std::memset(out[1], 0, static_cast<std::size_t>(n) * sizeof(float));
}
} // namespace

TrackVoice::~TrackVoice()
{
    endHead(main_);
    endHead(tail_);
    endHead(cut_);
    if (sample_ != nullptr)
        core::audio::release(sample_);
}

void TrackVoice::prepare(double sampleRate, int maxBlock)
{
    (void) maxBlock; // the engine block is fixed at 64 frames
    engineRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    fadeFrames_ = msToFrames(kFadeMs, engineRate_);
    endHead(main_);
    endHead(tail_);
    endHead(cut_);
    numEvents_ = 0;
    ended_ = false;
    mod_.fill(0.0f);
    stretch_.prepare();
    playMode_ = StretchMode::Off;
    deferred_ = false;
}

void TrackVoice::setSample(const SampleBuffer* buffer) noexcept
{
    if (buffer == sample_)
        return;
    fadeHead(main_, fadeFrames_); // the sounding note keeps its own reference while it fades
    if (buffer != nullptr)
        core::audio::retain(buffer);
    if (sample_ != nullptr)
        core::audio::release(sample_);
    sample_ = buffer;
}

void TrackVoice::start(const PlaybackParams& params, float modCents, int frame, float tailGain) noexcept
{
    queue({Event::Start, clampFrame(frame), 0, tailGain, modCents, params});
}

void TrackVoice::stop(int frame, float fadeMs) noexcept
{
    queue({Event::Stop, clampFrame(frame), msToFrames(fadeMs, engineRate_), 0.0f, 0.0f, {}});
}

void TrackVoice::queue(const Event& e) noexcept
{
    if (numEvents_ == kMaxEventsPerBlock)
        --numEvents_; // the last one is replaced
    int i = numEvents_++;
    while (i > 0 && events_[static_cast<std::size_t>(i - 1)].frame > e.frame)
    {
        events_[static_cast<std::size_t>(i)] = events_[static_cast<std::size_t>(i - 1)];
        --i;
    }
    events_[static_cast<std::size_t>(i)] = e;
}

void TrackVoice::setModCents(int source, float cents) noexcept
{
    if (source >= 0 && source < static_cast<int>(ModSource::Count))
        mod_[static_cast<std::size_t>(source)] = cents;
}

void TrackVoice::update(const PlaybackParams& params) noexcept
{
    if (params.bypass && ! bypass_)
        fadeHead(main_, fadeFrames_);
    bypass_ = params.bypass;
    const bool reverse = note_.region.reverse;
    note_ = params;
    note_.region.reverse = reverse;
    setHeadRegion(main_, note_.region);
}

void TrackVoice::setQuality(InterpolatorKind kind) noexcept { quality_ = kind; }

float TrackVoice::totalCents() const noexcept
{
    float c = note_.pitchCents;
    for (const float m : mod_)
        c += m;
    return c < -kMaxTotalCents ? -kMaxTotalCents : (c > kMaxTotalCents ? kMaxTotalCents : c);
}

Fixed TrackVoice::stepFor(float cents) const noexcept
{
    const SampleBuffer* b = main_.active ? main_.buf : sample_;
    const double ratio = b != nullptr && b->sampleRate > 0.0 ? b->sampleRate / engineRate_ : 1.0;
    const double rate = static_cast<double>(std::exp2(cents * (1.0f / 1200.0f))) * ratio; // once per note per block
    const Fixed s = kernels::toFixed(rate);
    return s < 1 ? 1 : s;
}

Fixed TrackVoice::stepForRate(double rate) const noexcept
{
    const SampleBuffer* b = main_.active ? main_.buf : sample_;
    const double ratio = b != nullptr && b->sampleRate > 0.0 ? b->sampleRate / engineRate_ : 1.0;
    const Fixed s = kernels::toFixed(rate * ratio);
    return s < 1 ? 1 : s;
}

void TrackVoice::updateRate() noexcept
{
    const float c = totalCents();
    switch (playMode_)
    {
        case StretchMode::Off:
            if (c != appliedCents_)
            {
                appliedCents_ = c;
                main_.step = stepFor(c);
            }
            break;
        case StretchMode::MasterTempo:
            stretch_.blockStart(stretch_.plan(StretchMode::MasterTempo, c, main_.buf));
            if (stretch_.active())
                break;
            playMode_ = StretchMode::Resample; // the backend became "none": the note continues resampled
            appliedRate_ = 0.0;
            [[fallthrough]];
        case StretchMode::Resample:
        {
            const double rate = stretch_.plan(StretchMode::Resample, c, main_.buf).rate;
            if (rate != appliedRate_)
            {
                appliedRate_ = rate;
                main_.step = stepForRate(rate);
            }
            break;
        }
    }
}

void TrackVoice::handOffToTail(float gainScale) noexcept
{
    if (! main_.active)
        return;
    if (stretch_.active()) // the stretcher stays with the voice; its next 2 ms go to the stretch tail
    {
        const int left = main_.fadeLeft;
        float* scratch[2] = {scratchB_[0], scratchB_[1]};
        stretch_.handOff(main_, main_.gain * (gainScale > 0.0f ? gainScale : 0.0f),
                         left > 0 && left < fadeFrames_ ? left : fadeFrames_, scratch);
        endHead(main_);
        return;
    }
    if (tail_.active) // a retrigger inside the previous tail's 2 ms: cut that tail with a 64-frame fade
    {
        endHead(cut_);
        cut_ = tail_;
        tail_ = Head {};
        fadeHead(cut_, kSeamFrames);
    }
    const int left = main_.fadeLeft;
    tail_ = main_; // the buffer reference moves with the head
    main_ = Head {};
    tail_.gain *= gainScale > 0.0f ? gainScale : 0.0f;
    tail_.fadeLeft = 0;
    if (tail_.gain <= 0.0f)
    {
        endHead(tail_);
        return;
    }
    fadeHead(tail_, left > 0 && left < fadeFrames_ ? left : fadeFrames_);
}

void TrackVoice::apply(const Event& e) noexcept
{
    if (e.kind == Event::Stop)
    {
        fadeHead(main_, e.fadeFrames);
        deferred_ = false;
        return;
    }
    handOffToTail(e.tailGain);
    note_ = e.params;
    mod_[static_cast<std::size_t>(ModSource::Note)] = e.modCents;
    ended_ = false;
    if (bypass_ || e.params.bypass || sample_ == nullptr)
        return;
    appliedCents_ = totalCents();
    Fixed step = stepFor(appliedCents_); // main_ is idle here: the step of the track's buffer
    playMode_ = note_.stretch;
    deferred_ = false;
    if (playMode_ == StretchMode::MasterTempo)
    {
        switch (stretch_.begin(stretch_.plan(StretchMode::MasterTempo, appliedCents_, sample_)))
        {
            case stretch::VoiceStretch::Start::Stretched: step = stepForRate(1.0); break;
            case stretch::VoiceStretch::Start::Deferred: // the priming budget is spent: frame 0 of the next block
                deferred_ = true;
                deferredEvent_ = e;
                deferredEvent_.frame = 0;
                return;
            case stretch::VoiceStretch::Start::Resample: playMode_ = StretchMode::Resample; break;
        }
    }
    if (playMode_ == StretchMode::Resample)
    {
        appliedRate_ = stretch_.plan(StretchMode::Resample, appliedCents_, sample_).rate;
        step = stepForRate(appliedRate_);
    }
    beginHead(main_, sample_, note_.region, step, quality_);
}

void TrackVoice::renderRange(float* const dst[2], float* const tail[2], int from, int to) noexcept
{
    const int n = to - from;
    if (n <= 0)
        return;
    float* d[2] = {dst[0] + from, dst[1] + from};
    float* scratchB[2] = {scratchB_[0], scratchB_[1]};
    if (main_.active)
    {
        const int m = main_.fadeLeft > 0 && main_.fadeLeft < n ? main_.fadeLeft : n;
        const bool stretched = stretch_.active();
        bool drained = false;
        if (stretched) // the head feeds the stretcher; the region ends when its output has played out
            drained = stretch_.render(main_, d, m, scratchB);
        else
            renderHead(main_, d, m, scratchB); // a fade ending inside the range: applyGain zeroes the rest
        applyGain(main_, d, n);
        if (main_.active && (stretched ? drained : main_.finished))
        {
            ended_ = true;
            endHead(main_);
        }
        if (stretched && ! main_.active)
            stretch_.end();
    }
    else
        zero(d, n);

    float* t[2] = {tail[0] + from, tail[1] + from};
    float* s[2] = {scratchA_[0], scratchA_[1]};
    for (Head* h : {&tail_, &cut_})
    {
        if (! h->active)
            continue;
        const int m = h->fadeLeft > 0 && h->fadeLeft < n ? h->fadeLeft : n;
        renderHead(*h, s, m, scratchB);
        const bool finished = h->finished;
        addWithGain(*h, t, s, m);
        if (h->active && finished)
            endHead(*h);
    }
    stretch_.addTail(t, n);
}

void TrackVoice::render(float* dstL, float* dstR, float* tailL, float* tailR) noexcept
{
    float* dst[2] = {dstL, dstR};
    float* tail[2] = {tailL, tailR};
    if (deferred_) // a MASTER TEMPO start the previous block's priming budget could not take
    {
        const Event e = deferredEvent_;
        deferred_ = false;
        apply(e);
    }
    if (main_.active) // block-start pitch: one exp2f when the cents changed (OFF), tempo / stretcher ratios
        updateRate();
    int from = 0;
    for (int i = 0; i < numEvents_; ++i)
    {
        const Event& e = events_[static_cast<std::size_t>(i)];
        renderRange(dst, tail, from, e.frame);
        apply(e);
        from = e.frame;
    }
    numEvents_ = 0;
    renderRange(dst, tail, from, kBlockFrames);
}

std::int64_t TrackVoice::position() const noexcept
{
    if (! main_.active)
        return -1;
    const std::int64_t p = main_.position();
    return p < 0 ? 0 : p;
}

float TrackVoice::playPosition() const noexcept
{
    if (! main_.active || main_.buf == nullptr || main_.buf->frames <= 0)
        return -1.0f;
    return static_cast<float>(static_cast<double>(position()) / static_cast<double>(main_.buf->frames));
}
} // namespace engine::voice
