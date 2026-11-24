// The transport clock (spec 02-features/sync-and-transport.md SYN-001/002/005,
// sequencer.md SEQ-003/006/044, 01-architecture/audio-engine.md step 3). Audio thread only; no allocation, no locks.
//
// Drift-free, sample-accurate PPQ (SEQ-003): the position is anchor + framesSinceAnchor × ppqPerFrame, where the
// anchor is re-based only when the rate changes — tempo, nudge — or the position jumps. A re-base keeps the anchor as
// hi + lo: hi = the PPQ the last block reported as its end, lo = the rounding remainder (two-sum), so consecutive
// blocks satisfy p0' = p1 exactly and no error accumulates over hours, even with a tempo change every block. Doubles
// are used per block, never per sample.
//
// Commands (TransportCmd; applied by the step-1 FIFO drain, i.e. at the next block start):
//   Play(bars)  Stopped/Paused → from PPQ 0; bars 1–2 (clamped) first run a pre-count from −4·bars to 0 (PreCount:
//               ctx.playing false, BlockBuffers::timing.preCount true, the metronome clicks); playing / pre-count: no-op
//   Continue    Stopped/Paused → Playing from the current position (a pause or a Locate); otherwise no-op
//   Stop(0)     pause: Playing → Paused, the position is kept; PreCount → Stopped at 0; otherwise no-op   (= Pause)
//   Stop(1)     Playing/Paused/PreCount → Stopped, back to PPQ 0 (step 1), tails ring; when already Stopped it is the
//               spec's second STOP and acts as Stop(2) (SYN-001; the FX-036 rule)
//   Stop(2)     hard stop: Stopped at 0 and hardStop for one block (voices fade, FX tails are cut)       (= HardStop)
//   SetTempo    20.0–300.0 BPM on the 0.1 grid (SYN-002); takes effect at the block start, the position is kept
//   Locate(ppq) jumps to ppq ≥ 0 in any state but PreCount (ignored there)
// startAt(ppq, frame) (audio thread, not a command): Stopped/Paused → Playing, `ppq` at frame `frame` of the next
//             block — an external clock's Start / Continue lands on its tick's sample, not on the block start. The
//             frames before it are a silent lead-in (PreCount state for under one block: no click, nothing plays).
//
// Per block (advance): ctx.ppqStart/ppqEnd = [p0, p1) of this block (equal while stopped or paused), ctx.bpm = the
// effective tempo (nudge applied), ctx.playing, ctx.startedThisBlock / stoppedThisBlock (Playing entered / left).
// A stop and a start applied between two blocks (MIDI Start while playing: Stop, then startAt) is a restart:
// startedThisBlock is set (the start resets run), stoppedThisBlock is not (the block plays), and restartedThisBlock()
// tells the stop's consumers (MIDI Stop out, the hanging-note guard).
// The block in which a pre-count reaches 0 is a Playing block with ppqStart < 0 < ppqEnd: playback starts on frame
// startFrame() = frameOf(0, p0, ppqPerFrame) (176,400 frames after a 2-bar pre-count at 120 BPM, 44.1 kHz).
#pragma once

#include "engine/EngineApi.h"

#include <cstdint>

namespace engine::transport
{
enum class State : std::uint8_t
{
    Stopped,
    Playing,
    Paused,
    PreCount
};

// External tempo/phase correction (SYN-012): called once per block before the position advances, with the block's
// start position and the current rate (quarters per frame); it may change the effective BPM (`bpm`, in and out) and
// request a phase jump in quarters (`phase`, 0 = none). Null = internal clock.
using CorrectionFn = void (*)(void* user, double ppq, double ppqPerFrame, double& bpm, double& phase) noexcept;

class Transport
{
public:
    static constexpr int kMaxPreCountBars = 2;

    void prepare(double sampleRate) noexcept;              // Stopped at 0; tempo, nudge and the hook are kept
    void command(const TransportCmd&) noexcept;            // from the FIFO drain
    void setTempo(double bpm) noexcept;                    // = SetTempo (ParamChange transport.tempo)
    void setNudge(int direction, float amount) noexcept;   // SYN-005: tempo × (1 + direction · amount) while ≠ 0
    void setCorrection(CorrectionFn fn, void* user) noexcept
    {
        correction_ = fn;
        correctionUser_ = user;
        rebase();
        syncBpm_ = 0.0;
        updateRate();
    }
    // See above (frame: fractional, 0 ≤ frame < 64); syncBpm > 0: the correction hook's tempo from now on (the
    // followed clock's), so the lead-in is measured at the rate the transport will run at.
    void startAt(double ppq, double frame, double syncBpm = 0.0) noexcept;
    void advance(BlockContext&) noexcept;                  // fills ctx (ppqStart, ppqEnd, bpm, playing, edges)

    State state() const noexcept { return state_; }
    double ppq() const noexcept { return position(0); }     // the next block's p0
    double bpm() const noexcept { return bpm_; }            // the stored tempo (the display)
    double effectiveBpm() const noexcept
    {
        return (syncBpm_ > 0.0 ? syncBpm_ : bpm_) * (1.0 + nudgeDir_ * static_cast<double>(nudgeAmount_));
    }
    double ppqPerFrame() const noexcept { return ppf_; }    // effective, quarters per frame
    double sampleRate() const noexcept { return sampleRate_; }
    int nudge() const noexcept { return nudgeDir_; }

    // Edges of the last advance() (one block each).
    bool hardStopThisBlock() const noexcept { return hardStopEdge_; }
    bool locatedThisBlock() const noexcept { return locatedEdge_; } // the position jumped (Locate, Stop to 0, Play)
    bool restartedThisBlock() const noexcept { return restartEdge_; }  // stopped and started again (see above)
    bool preCountThisBlock() const noexcept { return preCountBlock_; } // the block was (entirely) pre-count
    int startFrame() const noexcept { return startFrame_; }          // frame of the start edge (0 unless pre-count)
    double startPpq() const noexcept { return startPpq_; }           // position on the start edge (its block)

private:
    double position(std::int64_t extraFrames) const noexcept; // anchor + frames · ppqPerFrame (Transport.cpp)
    void rebase() noexcept;                   // fold the frames into the anchor (before a rate change)
    void locate(double ppq) noexcept;         // anchor = ppq exactly
    void updateRate() noexcept;               // ppf_ from bpm_, nudge, sample rate (after rebase)
    void stopAtZero(bool hard) noexcept;

    double sampleRate_ = 44100.0;
    double bpm_ = 120.0;
    double syncBpm_ = 0.0; // the correction hook's tempo, 0 = the stored tempo
    int nudgeDir_ = 0;
    float nudgeAmount_ = 0.03f;
    double ppf_ = 120.0 / (60.0 * 44100.0);
    double anchorHi_ = 0.0, anchorLo_ = 0.0;   // anchor PPQ as an unevaluated sum hi + lo
    std::int64_t frames_ = 0;                  // frames advanced since the anchor
    State state_ = State::Stopped;
    bool wasPlaying_ = false;                  // state_ was Playing at the end of the previous block
    bool hardStopPending_ = false, locatedPending_ = false, restartPending_ = false;
    bool hardStopEdge_ = false, locatedEdge_ = false, preCountBlock_ = false, restartEdge_ = false;
    int startFrame_ = 0;
    double startPpq_ = 0.0; // where PreCount ends (0 after a counted pre-count, the target of startAt)
    bool leadIn_ = false;   // the PreCount is startAt's lead-in, not a counted pre-count
    CorrectionFn correction_ = nullptr;
    void* correctionUser_ = nullptr;
};
} // namespace engine::transport
