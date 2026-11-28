// The source of one track (step 8a of audio-engine.md): Sample = TrackVoice × AmpEnvelope (lane `track` of the
// engine's AmpEnvelopeBank), Thru = the audio input with no envelope, MIDI / Instrument = silence. Real-time:
// everything but prepare()/attach() is noexcept, allocation-free and lock-free; one per track, a plain member array
// of the engine.
//
// Per block (the TriggerRouter calls onEvent()/choke() in frame order, then the engine calls render()):
//   Full       envelope trigger (gate = pad/MIDI hit on a GATE track, TRK-010/011; sequencer, retrigger and repeat
//              hits are One Shot, SEQ-015) + voice start at the event frame with the step's playback locks (SEQ-021)
//              and the pad-scale / MIDI note offset in cents (PLY-022)
//   Half       envelope trigger only, and only while the voice sounds (SEQ-011: the sample is not restarted)
//   LockOnly   the locks only (envelope release lock, playback locks of the sounding note)
//   Release    gate off (GATE pad up / MIDI note off): the envelope enters its release at the event frame
//   Stop       panic / choke: the voice fades out over 2 ms (PLY-042), the envelope is dropped once it is silent
// Plugin tracks send Full / Half as note on, Release as note off and Stop as all notes off, at the key plus TRANSPOSE.
// render(): when the block has a voice start, the envelope's 64 gains are rendered first so the old note moves to
// the voice's release tail with the gain it had just before the start frame (sample-accurate PLY-042 handoff); else
// AmpEnvelopeBank::process multiplies in place (constant segments cost one scalar multiply or nothing). The voice's
// tails (post-envelope) are added last. The envelope finishing stops the voice; a voice that stopped (sample end,
// choke, mode change, no sample) resets its envelope, so idle tracks cost one branch.
//
// Mode changes (TRK-002): the sounding source fades out over 2 ms and the new one fades in over 2 ms (Thru input).
#pragma once

#include "core/audio/PluginNode.h"
#include "engine/EngineApi.h"
#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/trigger/TrackParams.h"
#include "engine/trigger/TriggerEvent.h"
#include "engine/voice/PlaybackParams.h"
#include "engine/voice/SliceMap.h"
#include "engine/voice/TrackVoice.h"

#include <array>
#include <cstdint>
#include <utility>

namespace engine::trigger
{
class TrackSource
{
public:
    static constexpr int kMaxStarts = 8; // voice starts per block (≥ TrackVoice::kMaxEventsPerBlock)

    TrackSource() noexcept; // out of line (GCC 11: an inlined array of 16 of them stalls the engine's -O2 build)
    ~TrackSource();
    TrackSource(const TrackSource&) = delete;
    TrackSource& operator= (const TrackSource&) = delete;

    // ---- message thread ----
    void prepare(double sampleRate, int maxBlock);
    void attach(modulation::AmpEnvelopeBank* bank, int lane) noexcept;

    // ---- audio thread (block start / router) ----
    void setMode(TrackMode) noexcept;                        // TRK-002: stops the sound (2 ms)
    void setInputSource(InputSource s) noexcept { input_ = s; }
    void setSample(const voice::SampleBuffer* b) noexcept { voice_.setSample(b); }
    void setPlugin(core::audio::PluginAudioNode* p) noexcept;
    void detachPluginForMove() noexcept { plugin_ = nullptr; }
    // Plugin notes sound the track's TRANSPOSE (seq.transpose, SEQ-035) above their key; the engine sets it before the
    // router's block. A note ends on the pitch it started at, even when TRANSPOSE moves while it is held.
    void setNoteTranspose(int semitones) noexcept { noteTranspose_ = semitones; }
    // A track swap moves the instrument to the other source, so its sounding notes go with it.
    void exchangeHeldNotes(TrackSource& other) noexcept { std::swap(sentNotes_, other.sentNotes_); }
    void setPlayback(const voice::PlaybackParams& p) noexcept; // the track's current playback parameters
    // The track's installed slice table (null = the default 16-slice grid). A Full trigger with a slice
    // lock (ParamIndex 22: SLICE pad mode, a sequencer step) plays [start, next start) of that slice with the track's
    // envelope, pitch, loop and FX (SLC-004; the loop runs inside the slice); a slice the grid lacks is silent.
    void setSliceMap(const voice::SliceMap* map) noexcept { sliceMap_ = map; }
    int playingSlice() const noexcept { return voice_.mainActive() ? noteSlice_ : -1; } // −1 = whole region / idle
    void onEvent(const TriggerEvent&, TriggerMode) noexcept;
    void stopPlayback(bool loopsOnly) noexcept;
    void choke(int frame) noexcept; // TRK-020: 2 ms fade of the sounding note, pending starts of this block dropped
    // Writes 64 frames into l/r (overwritten). inL/inR: this block's audio input (null = none). false = silent block
    // (l/r untouched).
    bool render(const BlockContext&, const float* inL, const float* inR, float* l, float* r) noexcept;
    bool render(const BlockContext& ctx, float* l, float* r) noexcept { return render(ctx, nullptr, nullptr, l, r); }

    // ---- state ----
    TrackMode mode() const noexcept { return mode_; }
    bool hasSample() const noexcept { return voice_.sample() != nullptr; }
    bool hasPlugin() const noexcept { return plugin_ != nullptr; }
    bool sounding() const noexcept
    {
        return voice_.active() || thruGain_ > 0.0f ||
               (mode_ == TrackMode::Plugin && plugin_ != nullptr && plugin_->active());
    }
    float playPosition() const noexcept { return voice_.playPosition(); } // PlayheadFeed (PLY-045)
    voice::TrackVoice& voice() noexcept { return voice_; }
    const voice::TrackVoice& voice() const noexcept { return voice_; }
    // The pitch (cents) the track's notes play before modulation: the stored pitch or the last trigger's step lock
    // (the ModMatrix's base for playback.pitch, FX-084: "the stored or locked value").
    float basePitchCents() const noexcept { return basePitch_; }

private:
    struct Start
    {
        std::uint8_t frame;
        float cents;
        voice::PlaybackParams params;
        std::int8_t slice; // −1 = the track's region
    };

    int resolveSlice(const TriggerEvent& e, std::uint64_t mask, const float* values, const std::uint8_t* change) noexcept;
    voice::PlaybackParams withNoteSlice(voice::PlaybackParams p) const noexcept; // the sounding slice's region
    bool applySlice(voice::PlaybackParams& p, int slice) const noexcept;

    bool renderThru(const float* inL, const float* inR, float* l, float* r, bool add) noexcept;

    modulation::AmpEnvelopeBank* bank_ = nullptr;
    int lane_ = 0;
    voice::TrackVoice voice_;
    voice::PlaybackLocks locks_;
    voice::PlaybackParams base_ {};
    float basePitch_ = 0.0f; // basePitchCents()
    TrackMode mode_ = TrackMode::Sample;
    InputSource input_ = InputSource::Stereo;
    bool thruEnvelope_ = false; // the bank lane is in its Thru (no envelope) state
    float thruGain_ = 0.0f, thruStep_ = 1.0f / 88.0f;
    std::array<Start, kMaxStarts> starts_; // written before read (no braces: GCC 11 build time)
    int numStarts_ = 0;
    const voice::SliceMap* sliceMap_ = nullptr;
    core::audio::PluginAudioNode* plugin_ = nullptr;
    int noteTranspose_ = 0;
    std::array<std::uint8_t, 128> sentNotes_ {}; // per key (60 + semitones): the note the plugin got + 1, 0 = none
    int heldSlice_ = -1, liveSlice_ = -1, noteSlice_ = -1; // Hold lock, last pad slice (Repeat hits), sounding
    alignas(16) float gain_[kBlockFrames] {};
    alignas(16) float tailL_[kBlockFrames] {};
    alignas(16) float tailR_[kBlockFrames] {};
};
} // namespace engine::trigger
