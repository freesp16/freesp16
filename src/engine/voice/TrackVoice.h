// The sample player of one Sample track (PLY-001…046; audio-engine.md "Polyphony": one voice per track + a
// release-tail voice for click-free retrigger). Real-time: everything but prepare() is
// noexcept, allocation-free and lock-free; state is fixed-size members.
//
// Per block (the caller is TrackSource):
//   1  setSample(slot content)            the track's SampleBuffer (SwapSlot::TrackSample); a change fades out the
//                                         sounding note (2 ms) and plays later triggers from the new buffer
//   2  update(params) / setModCents(…)    knob changes and modulation for the sounding note (pitch: once per block)
//   3  start(…) / stop(…) per event       queued at their frame (0–63), applied in frame order by render()
//   4  render(dst, tail)                  dst = 64 frames of the main voice, pre-envelope (overwritten);
//                                         tail += the faded release tails, post-envelope (their gain is frozen)
//   5  position() / playPosition()        PLY-045, for PlayheadFeed
//
// Retrigger (PLY-042): the sounding voice moves to the tail with gain `tailGain` (the envelope's gain of the old note
// at the trigger frame) and fades out linearly over 2 ms; the new note starts sample-accurately at its frame. A tail
// still fading when the next retrigger comes is cut with a 64-frame fade. stop() fades the main voice in place (the
// envelope keeps applying), so a choke or a hard stop is click-free too. Bypass (PLY-044) silences the track (2 ms
// fade-out) and starts no new notes; the parameters are kept.
//
// Pitch (PLY-020/022): cents = note pitch (params, with locks) + start()'s modCents (pad scale / MIDI note) + the
// setModCents sources; rate = 2^(cents/1200) · bufferRate/engineRate, one exp2f per active note per block.
//
// TIME STRETCH (PLY-030…034; engine/voice/stretch/VoiceStretch.h), fixed per note like reverse: OFF as
// above; RESAMPLE plays at projectBpm / sampleBpm (PITCH ignored, PLY-032); MASTER TEMPO reads the head at unity into
// the voice's lane of the engine's StretcherBank (timeRatio = sampleBpm / projectBpm, pitch ±12 st). Without a lane
// (no bank, backend "none") or while the governor holds new MASTER TEMPO notes, MASTER TEMPO plays as RESAMPLE; a
// start beyond the block's priming budget waits for the next block. A stretched note's retrigger fade is rendered
// ahead into the stretch tail (the stretcher cannot be shared with a tail head).
#pragma once

#include "engine/EngineApi.h"
#include "engine/voice/VoiceHead.h"
#include "engine/voice/stretch/VoiceStretch.h"

#include <array>
#include <cstdint>

namespace engine::voice
{
enum class ModSource : std::uint8_t // PLY-022 contributions, added in cents
{
    Note,      // pad scale / MIDI note offset (start()'s modCents)
    Transpose, // sequencer transpose
    Strip,     // touch strip in PITCH mode
    Lfo,       // LFO → pitch
    Count
};

class TrackVoice
{
public:
    static constexpr int kMaxEventsPerBlock = 8; // further events of one block replace the last one
    static constexpr float kFadeMs = 2.0f;       // PLY-042

    TrackVoice() = default;
    ~TrackVoice();
    TrackVoice(const TrackVoice&) = delete;
    TrackVoice& operator= (const TrackVoice&) = delete;

    // Message thread (device stopped): sets the engine rate and silences the voice. Allocates nothing.
    void prepare(double sampleRate, int maxBlock);

    // ---- audio thread, block start ----
    void setSample(const SampleBuffer*) noexcept; // retains the new buffer, releases the old one
    // FULL trigger at `frame` (0–63): the sounding voice moves to the tail with gain `tailGain` and fades out over
    // 2 ms; the main voice restarts at region.start (or the region end when reversed).
    void start(const PlaybackParams&, float modCents, int frame, float tailGain) noexcept;
    void stop(int frame, float fadeMs = kFadeMs) noexcept; // choke, hard stop, mode change
    void setModCents(int source, float cents) noexcept;   // ModSource index
    // The track's current parameters for the sounding note: pitch, region and loop apply at once (position kept),
    // reverse at the next start; bypass fades the note out.
    void update(const PlaybackParams&) noexcept;
    void setQuality(InterpolatorKind) noexcept; // hermite4 (default) or sinc16 (render mode); from the next note
    // The engine's stretcher bank and this voice's lane (message thread, before the device starts),
    // and every block the transport tempo and the sample's BPM (0 = unknown, PLY-031 estimate).
    void attachStretch(stretch::StretcherBank* bank, int lane) noexcept { stretch_.attach(bank, lane); }
    void setTempo(double projectBpm, float sampleBpm) noexcept { stretch_.setTempo(projectBpm, sampleBpm); }

    // Writes 64 frames of the main voice (pre-envelope) to dst, adds the faded tails into tail.
    void render(float* dstL, float* dstR, float* tailL, float* tailR) noexcept;

    // ---- state ----
    bool active() const noexcept // anything sounding
    {
        return main_.active || tail_.active || cut_.active || stretch_.tailActive();
    }
    bool mainActive() const noexcept { return main_.active; }
    bool stretching() const noexcept { return stretch_.active(); }            // the main note runs a stretcher
    StretchMode playMode() const noexcept { return playMode_; }               // the main note's effective mode
    bool ended() const noexcept { return ended_; } // the non-looping region of the last note finished
    std::int64_t position() const noexcept;        // main voice frame, −1 when idle (PLY-045)
    float playPosition() const noexcept;           // position / sample frames (0–1), −1 when idle (PlayheadFeed)
    const SampleBuffer* sample() const noexcept { return sample_; }
    int fadeFrames() const noexcept { return fadeFrames_; } // 2 ms at the engine rate

private:
    struct Event
    {
        enum Kind : std::uint8_t { Start, Stop } kind;
        std::uint8_t frame;
        int fadeFrames;
        float tailGain, modCents;
        PlaybackParams params;
    };

    void queue(const Event& e) noexcept;
    void apply(const Event& e) noexcept;
    void handOffToTail(float gainScale) noexcept;
    void renderRange(float* const dst[2], float* const tail[2], int from, int to) noexcept;
    Fixed stepFor(float noteCents) const noexcept;
    Fixed stepForRate(double rate) const noexcept; // rate × bufferRate / engineRate
    void updateRate() noexcept;                    // block start: the main note's step / stretcher ratios
    float totalCents() const noexcept;

    double engineRate_ = 44100.0;
    int fadeFrames_ = 88;
    InterpolatorKind quality_ = InterpolatorKind::Hermite4;
    const SampleBuffer* sample_ = nullptr; // the track's buffer (one reference)

    Head main_, tail_, cut_;
    PlaybackParams note_ {};  // the sounding note's parameters (with its locks)
    std::array<float, static_cast<int>(ModSource::Count)> mod_ {};
    float appliedCents_ = 0.0f;
    bool bypass_ = false, ended_ = false;

    std::array<Event, kMaxEventsPerBlock> events_ {};
    int numEvents_ = 0;

    stretch::VoiceStretch stretch_;
    StretchMode playMode_ = StretchMode::Off;  // the main note's effective mode
    double appliedRate_ = 0.0;                 // RESAMPLE: the rate of main_.step
    bool deferred_ = false;                    // a MASTER TEMPO start waits for the next block's priming budget
    Event deferredEvent_ {};

    alignas(16) float scratchA_[2][kBlockFrames] {};
    alignas(16) float scratchB_[2][kBlockFrames] {};
};
} // namespace engine::voice
