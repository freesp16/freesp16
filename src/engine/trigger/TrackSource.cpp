#include "engine/trigger/TrackSource.h"

#include "engine/dsp/simd/Kernels.h"

#include <cmath>
#include <cstring>

namespace engine::trigger
{
namespace
{
constexpr float kFadeMs = voice::TrackVoice::kFadeMs; // TRK-002 / TRK-020: 2 ms

std::uint8_t clampFrame(int f) noexcept
{
    return static_cast<std::uint8_t>(f < 0 ? 0 : (f > kBlockFrames - 1 ? kBlockFrames - 1 : f));
}
} // namespace

TrackSource::TrackSource() noexcept = default;
TrackSource::~TrackSource() = default;

void TrackSource::prepare(double sampleRate, int maxBlock)
{
    voice_.prepare(sampleRate, maxBlock);
    locks_.reset();
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const long fade = std::lround(static_cast<double>(kFadeMs) * 0.001 * rate);
    thruStep_ = 1.0f / static_cast<float>(fade < 1 ? 1 : fade);
    thruGain_ = mode_ == TrackMode::Thru ? 1.0f : 0.0f;
    numStarts_ = 0;
    thruEnvelope_ = false;
    if (bank_ != nullptr)
    {
        bank_->setThru(lane_, mode_ == TrackMode::Thru);
        thruEnvelope_ = mode_ == TrackMode::Thru;
    }
}

void TrackSource::attach(modulation::AmpEnvelopeBank* bank, int lane) noexcept
{
    bank_ = bank;
    lane_ = lane;
}

void TrackSource::setMode(TrackMode m) noexcept
{
    if (m == mode_)
        return;
    voice_.stop(0, kFadeMs); // the sounding note fades out (the envelope keeps applying while it does)
    numStarts_ = 0;
    if (mode_ == TrackMode::Plugin && plugin_ != nullptr)
        plugin_->allNotesOff(0);
    if (mode_ == TrackMode::Thru && bank_ != nullptr && thruEnvelope_)
    {
        bank_->setThru(lane_, false); // back to an idle envelope; the input fades out below
        thruEnvelope_ = false;
    }
    mode_ = m;
}

void TrackSource::setPlugin(core::audio::PluginAudioNode* p) noexcept
{
    if (p == plugin_)
        return;
    if (plugin_ != nullptr)
        plugin_->allNotesOff(0);
    plugin_ = p;
}

void TrackSource::setPlayback(const voice::PlaybackParams& p) noexcept
{
    base_ = p;
    const auto resolved = locks_.resolve(base_, 0, nullptr, nullptr); // held locks stay on the sounding note
    basePitch_ = resolved.pitchCents;
    voice_.update(withNoteSlice(resolved));
}

// the slice a Full trigger plays (−1 = the track's region). A slice lock with change Hold / Glide stays
// for the following notes (SEQ-021); a Repeat hit replays the last pad's slice (REPEAT in SLICE pad mode; our choice).
int TrackSource::resolveSlice(const TriggerEvent& e, std::uint64_t mask, const float* values,
                              const std::uint8_t* change) noexcept
{
    constexpr std::uint64_t bit = std::uint64_t {1} << voice::kLockSlotSlice;
    int slice = heldSlice_;
    if (values != nullptr && (mask & bit) != 0)
    {
        const float v = values[voice::kLockSlotSlice];
        slice = v < 0.0f ? -1 : static_cast<int>(v + 0.5f);
        const bool hold = change != nullptr && change[voice::kLockSlotSlice] != 0;
        heldSlice_ = hold ? slice : -1;
    }
    if (e.source == TriggerSource::Pad)
        liveSlice_ = (mask & bit) != 0 ? slice : -1;
    else if (e.source == TriggerSource::Repeat && (mask & bit) == 0)
        slice = liveSlice_;
    return slice;
}

bool TrackSource::applySlice(voice::PlaybackParams& p, int slice) const noexcept
{
    if (slice < 0)
        return true;
    voice::SliceRegion r;
    if (! voice::sliceRegion(sliceMap_, voice_.sample() != nullptr ? voice_.sample()->frames : 0, slice, r))
        return false;
    p.region.start = r.start;
    p.region.loopStart = r.start; // the loop runs inside the slice (SLC-004), from the slice start (our choice)
    p.region.length = r.end - r.start;
    return true;
}

voice::PlaybackParams TrackSource::withNoteSlice(voice::PlaybackParams p) const noexcept
{
    if (noteSlice_ >= 0 && voice_.mainActive())
        applySlice(p, noteSlice_);
    return p;
}

void TrackSource::onEvent(const TriggerEvent& e, TriggerMode triggerMode) noexcept
{
    if (mode_ == TrackMode::Plugin)
    {
        if (plugin_ == nullptr)
            return;
        const int frame = clampFrame(e.frame);
        const int rawNote = 60 + static_cast<int>(e.semitones);
        const int note = rawNote < 0 ? 0 : (rawNote > 127 ? 127 : rawNote);
        const float vel = static_cast<float>(e.velocity) * (1.0f / 127.0f);
        switch (e.kind)
        {
        case TriggerKind::Full:
        case TriggerKind::Half:
            plugin_->noteOn(frame, note, vel > 1.0f ? 1.0f : (vel < 0.0f ? 0.0f : vel));
            return;
        case TriggerKind::Release:
            // The router retains the pitch from the pad press. Other notes on this track may still be held.
            plugin_->noteOff(frame, note);
            return;
        case TriggerKind::Stop:
            plugin_->allNotesOff(frame);
            return;
        case TriggerKind::LockOnly:
            return;
        }
        return;
    }
    if (mode_ != TrackMode::Sample || bank_ == nullptr)
        return; // Thru: no envelope; MIDI / Instrument: no audio source (the MIDI stage plays them)
    const int frame = clampFrame(e.frame);
    const bool live = e.source == TriggerSource::Pad || e.source == TriggerSource::Midi;
    bool gate = live && triggerMode == TriggerMode::Gate; // SEQ-015: only pad / MIDI hits honour GATE
    float lockedMode = 0.0f;
    if (live && e.locks != nullptr && e.locks->get(kLockSlotTriggerMode, lockedMode))
        gate = lockedMode >= 0.5f;
    const std::uint64_t mask = e.locks != nullptr ? e.locks->mask : 0;
    const float* values = e.locks != nullptr ? e.locks->values : nullptr;
    const std::uint8_t* change = e.locks != nullptr ? e.locks->change : nullptr;
    switch (e.kind)
    {
        case TriggerKind::Full:
        {
            voice::PlaybackParams params = locks_.resolve(base_, mask, values, change);
            basePitch_ = params.pitchCents;
            const int slice = resolveSlice(e, mask, values, change);
            if (! applySlice(params, slice))
                return; // the slice does not exist on this grid: no note
            bank_->trigger(e, gate);
            if (numStarts_ == kMaxStarts)
                --numStarts_; // the last one is replaced (TrackVoice keeps 8 events per block too)
            Start& s = starts_[static_cast<std::size_t>(numStarts_++)];
            s.frame = static_cast<std::uint8_t>(frame);
            s.cents = static_cast<float>(e.semitones) * voice::kPitchCentsPerUnit;
            s.params = params;
            s.slice = static_cast<std::int8_t>(slice);
            return;
        }
        case TriggerKind::Half: // SEQ-011: the envelope restarts on the sounding voice, the sample continues
            if (! voice_.mainActive() && numStarts_ == 0)
                return;
            bank_->trigger(e, gate);
            if (mask != 0)
            {
                const auto resolved = locks_.resolve(base_, mask, values, change);
                basePitch_ = resolved.pitchCents;
                voice_.update(withNoteSlice(resolved));
            }
            return;
        case TriggerKind::LockOnly:
            bank_->trigger(e, false);
            if (mask != 0)
            {
                const auto resolved = locks_.resolve(base_, mask, values, change);
                basePitch_ = resolved.pitchCents;
                voice_.update(withNoteSlice(resolved));
            }
            return;
        case TriggerKind::Release:
            bank_->release(lane_, frame); // ignored unless the running envelope was started gated
            return;
        case TriggerKind::Stop: choke(frame); return;
    }
}

void TrackSource::stopPlayback(bool loopsOnly) noexcept
{
    voice_.stopPlayback(loopsOnly);
    if (! loopsOnly && mode_ == TrackMode::Plugin && plugin_ != nullptr)
    {
        plugin_->allNotesOff(0);
        plugin_->resetEffect(); // CLAP reset also clears instrument release tails.
    }
}

void TrackSource::choke(int frame) noexcept
{
    const int f = clampFrame(frame);
    if (mode_ == TrackMode::Plugin)
    {
        if (plugin_ != nullptr)
            plugin_->allNotesOff(f);
        return;
    }
    int kept = 0; // starts at or before the choke frame never sound; later ones (a re-hit) still do
    for (int i = 0; i < numStarts_; ++i)
        if (starts_[static_cast<std::size_t>(i)].frame > f)
            starts_[static_cast<std::size_t>(kept++)] = starts_[static_cast<std::size_t>(i)];
    numStarts_ = kept;
    if (voice_.active() || kept != 0)
        voice_.stop(f, kFadeMs);
}

bool TrackSource::renderThru(const float* inL, const float* inR, float* l, float* r, bool add) noexcept
{
    const float target = mode_ == TrackMode::Thru ? 1.0f : 0.0f;
    if (thruGain_ == 0.0f && target == 0.0f)
        return false;
    if (inL == nullptr)
    {
        thruGain_ = target; // no input: nothing to fade
        return false;
    }
    const float* srcL = inL;
    const float* srcR = inR != nullptr ? inR : inL;
    switch (input_)
    {
        case InputSource::Stereo: break;
        case InputSource::Left: srcR = srcL; break;
        case InputSource::Right: srcL = srcR; break;
        case InputSource::Mono: break; // handled below
    }
    const bool mono = input_ == InputSource::Mono;
    if (thruGain_ == target && ! mono && ! add)
    {
        std::memcpy(l, srcL, sizeof(float) * kBlockFrames); // unity pass-through, bit-exact (NFR-030)
        std::memcpy(r, srcR, sizeof(float) * kBlockFrames);
        return true;
    }
    float g = thruGain_;
    for (int i = 0; i < kBlockFrames; ++i)
    {
        if (g != target)
        {
            g += target > g ? thruStep_ : -thruStep_;
            g = target > 0.0f ? (g > 1.0f ? 1.0f : g) : (g < 0.0f ? 0.0f : g);
        }
        const float a = mono ? 0.5f * (srcL[i] + srcR[i]) : srcL[i];
        const float b = mono ? a : srcR[i];
        l[i] = (add ? l[i] : 0.0f) + a * g;
        r[i] = (add ? r[i] : 0.0f) + b * g;
    }
    thruGain_ = g;
    return true;
}

bool TrackSource::render(const BlockContext& ctx, const float* inL, const float* inR, float* l, float* r) noexcept
{
    if (numStarts_ != 0 && voice_.sample() == nullptr && ! voice_.active())
        numStarts_ = 0; // nothing to play (no sample): the track stays silent (and inactive) this block
    const bool voiced = voice_.active() || numStarts_ != 0;
    bool wrote = false;
    if (voiced && bank_ != nullptr)
    {
        if (numStarts_ != 0)
        {
            // The envelope first: the gain just before each start frame is the old note's tail gain (PLY-042).
            const float before = bank_->currentGain(lane_);
            bank_->renderGain(lane_, gain_);
            for (int i = 0; i < numStarts_; ++i)
            {
                const Start& s = starts_[static_cast<std::size_t>(i)];
                voice_.start(s.params, s.cents, s.frame, s.frame > 0 ? gain_[s.frame - 1] : before);
                noteSlice_ = s.slice; // later knob changes keep the sounding slice's region
            }
            numStarts_ = 0;
            std::memset(tailL_, 0, sizeof(tailL_));
            std::memset(tailR_, 0, sizeof(tailR_));
            voice_.render(l, r, tailL_, tailR_);
            simd::multiply(l, gain_, kBlockFrames);
            simd::multiply(r, gain_, kBlockFrames);
        }
        else
        {
            std::memset(tailL_, 0, sizeof(tailL_));
            std::memset(tailR_, 0, sizeof(tailR_));
            voice_.render(l, r, tailL_, tailR_);
            bank_->process(lane_, l, r);
        }
        simd::add(l, tailL_, kBlockFrames);
        simd::add(r, tailR_, kBlockFrames);
        wrote = true;

        if (! voice_.active())
            bank_->reset(lane_); // the note is over (sample end, choke, stop, no sample): nothing left to shape
        else if (voice_.mainActive() && bank_->finished(lane_))
            voice_.stop(0, 0.1f); // the envelope ended (silent from here): free the voice next block
    }
    else if (bank_ != nullptr && ! thruEnvelope_ && ! bank_->finished(lane_))
        bank_->reset(lane_); // events for a silent track (a release, a lock) must not reach the next note
    if (mode_ == TrackMode::Thru && ! voice_.active() && ! thruEnvelope_ && bank_ != nullptr)
    {
        bank_->setThru(lane_, true); // the old note is gone: the lane bypasses the envelope
        thruEnvelope_ = true;
    }
    if (renderThru(inL, inR, l, r, wrote))
        wrote = true;
    if (mode_ == TrackMode::Plugin && plugin_ != nullptr)
    {
        const core::audio::PluginProcessContext pctx{
            ctx.sampleRate, ctx.bpm,     ctx.ppqStart,         ctx.ppqEnd,
            ctx.frames,     ctx.playing, ctx.startedThisBlock, ctx.stoppedThisBlock};
        if (wrote)
        {
            if (plugin_->process(pctx, tailL_, tailR_))
            {
                simd::add(l, tailL_, kBlockFrames);
                simd::add(r, tailR_, kBlockFrames);
            }
        }
        else if (plugin_->process(pctx, l, r))
            wrote = true;
    }
    return wrote;
}
} // namespace engine::trigger
