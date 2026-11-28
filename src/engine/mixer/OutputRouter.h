// engine::mixer::OutputRouter — step 12 of the engine block (spec 02-features/mixer-and-routing.md MIX-030…033,
// 01-architecture/audio-engine.md "Outputs"; research docs/effects/signal-flow.md "Main output and headphones", "Device
// output stage").
//
//   M + 6CH (AssignMode::MasterPlus6): OUT 1-2 = the master. A track assigned to OUT 3-4 / 5-6 / 7-8 or a mono channel
//            3…8 stays in the master (the mixer's main bus) and is also added to that output (MIX-031).
//   8CH      (AssignMode::EightChannel): every track goes to one of OUT 1-2…7-8 or mono 1…8 (`--` = OUT 1-2);
//            OUT 1-2 carries only its assigned tracks, never the master bus (MIX-030).
//   The assign feed is the strip's post-fader, post-pan output (BlockBuffers::track after Mixer::processTracks): no
//   send, no Send/Master FX ever reach a direct output (FX-012). Mono = (L + R) / 2 on one channel (MIX-031).
//   A mode change is a 128-sample fade-out, the switch, a 128-sample fade-in on every router output (MIX-033, the
//   research Output12Router dip); an assignment change fades that track's feed out and in the same way. Both are
//   the research Selector (headphone_mix.py, F209/F212) operation for operation: a change during a fade-in fades out
//   over 128 frames from the gain reached; re-selecting the current mode during its fade-out fades back in from the
//   gain reached (app) — the reference stays at gain 0 until the next change (F209 quirk, setReferenceQuirk(true),
//   mode only; tests/golden/routing).
//   Logical outputs the device does not have (OutputRouter::setPresent) fold into Main (MIX-032): in M + 6CH nothing
//   changes (the track is in the master already), in 8CH the track is added to OUT 1-2 instead.
//
// What the rest of the engine reads:
//   resampleTap(ch)  the master after the master fader (+ aux, + master limiter), before the output trim and before
//                    the mute-all fade: the SOURCE RESAMPLE input of sampling (SMP-001) and of the render
//                    (REN-007). Valid from process() until the next process() (64 frames, ch 0 = L, 1 = R).
//   setMuteAll(on)   a 64-sample linear fade of every live output to silence and back (research BulkMute, render mode
//                    REN-005, panic); the resample tap is not muted (our choice: the offline render is its own Engine).
//
// Threads: prepare() on the message thread; everything else on the audio thread (the engine's step-1 drain calls the
// setters, step 12 calls process()). No allocation, no lock, no virtual call; the per-sample work is the NEON / scalar
// block kernels of OutputRouterKernels.h. Budget: ≤ 4 k A9 cycles per block worst case, ≤ 300 in the common
// case (M + 6CH, nothing assigned: OUT 1-2 is the master pointer, no copy).
#pragma once

#include "engine/EngineApi.h"
#include "engine/mixer/OutputRouterKernels.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <cstdint>
#include <span>

namespace engine::mixer
{
// The logical outputs: the first six are engine::OutPort / platform::AudioPort in the same order; Resample
// is the internal copy of the master for the sampler (never a device port).
enum class LogicalOut : std::uint8_t { Main, Out34, Out56, Out78, Headphones, AfsSend, Resample, Count };
static_assert(static_cast<int>(LogicalOut::Main) == static_cast<int>(OutPort::Main) &&
                  static_cast<int>(LogicalOut::Headphones) == static_cast<int>(OutPort::Phones) &&
                  static_cast<int>(LogicalOut::AfsSend) == static_cast<int>(OutPort::FilterSend) &&
                  static_cast<int>(LogicalOut::Resample) == static_cast<int>(OutPort::Count),
              "LogicalOut extends engine::OutPort");

enum class AssignMode : std::uint8_t { MasterPlus6, EightChannel }; // MIX-030 (core::model::AssignMode order)

// OUT ASSIGN of one track (MIX-031): pair 0…3 = OUT 1-2…7-8, or mono 0…7 = channel 1…8; both −1 = "--" (master only;
// OUT 1-2 in 8CH). pair wins when both are set.
struct OutAssign
{
    std::int8_t pair = -1;
    std::int8_t mono = -1;
    friend bool operator== (const OutAssign&, const OutAssign&) = default;
};
// core::model::OutputAssign order: 0 Master, 1 Out12 … 4 Out78, 5 Mono1 … 12 Mono8.
constexpr OutAssign outAssignFromIndex(int index) noexcept
{
    if (index >= 1 && index <= 4)
        return {static_cast<std::int8_t>(index - 1), -1};
    if (index >= 5 && index <= 12)
        return {-1, static_cast<std::int8_t>(index - 5)};
    return {};
}

// One logical output of the device's map (MIX-032): the platform's physical channels, −1 = absent. The engine only
// uses presence (it never knows the physical layout); the entries mirror platform::PortInfo.
struct ChannelMapEntry
{
    LogicalOut port;
    std::int8_t left = -1, right = -1;
};

constexpr int kRouterPairs = 4; // OUT 1-2, 3-4, 5-6, 7-8 (= LogicalOut Main … Out78)

// The tracks' assign feed: BlockBuffers::track after the strips, bit t of `valid` = track t's block holds its
// post-fader signal this block (Mixer::stripOutputMask(); a silent or fully muted track is not valid).
struct TrackOutputs
{
    const stages::StereoBlock* track = nullptr; // kTracks blocks
    std::uint32_t valid = 0;
};

// What the router produced this block, before the output trim (step 13 applies it). pair[p] points at the block OUT
// (2p+1)-(2p+2) plays, null = silent this block. The blocks live in the router (or, for OUT 1-2 in the common M + 6CH
// case, in BlockBuffers::master). channel(n) = one of the eight mono channels (n = 0…7), null = silent.
struct OutputBuffers
{
    std::array<const float*, 2 * kRouterPairs> ch {};
    const float* channel(int n) const noexcept { return n >= 0 && n < 2 * kRouterPairs ? ch[static_cast<std::size_t>(n)] : nullptr; }
    bool pairActive(int p) const noexcept { return ch[static_cast<std::size_t>(2 * p)] != nullptr; }
};

class OutputRouter
{
public:
    static constexpr int kFadeFrames = 128;    // MIX-033 / assignment changes: 128 out + 128 in
    static constexpr int kMuteAllFrames = 64;  // research BulkMute

    OutputRouter() noexcept;

    // Message thread (device stopped). `map` = the device's logical outputs (absent entries or ports fold into Main).
    void prepare(double sampleRate, std::span<const ChannelMapEntry> map) noexcept;

    // ---- audio thread (block start) ----
    void setMode(AssignMode) noexcept;               // MIX-030 (per scene): 128 + 128-sample dip (MIX-033)
    void swapAssignments(int a, int b) noexcept;
    void setAssign(int track, OutAssign) noexcept;   // MIX-031 (per scene and track, the current scene's)
    void setPresent(std::uint8_t pairMask) noexcept; // bit p = OUT pair p exists on the device (bit 0 always)
    void setMuteAll(bool) noexcept;                  // 64-sample fade (REN-005, panic)
    void setReferenceQuirk(bool on) noexcept { quirk_ = on; } // FX-052 reference profile: the F209 mode re-select quirk

    // Step 12. master = the mixer's master (BlockBuffers::master: post master fader, pre trim), two channels.
    void process(const TrackOutputs& tracks, const float* const* master, OutputBuffers& out) noexcept;

    const float* resampleTap(int ch) const noexcept { return ch == 1 ? tapR_ : tapL_; } // REN-007 / SMP-001

    // Step 13 helper: the mute-all gain of this block (null = 1 for every frame); `silent` = fully muted.
    const float* muteAllGains(bool& silent) const noexcept
    {
        silent = muteSilent_;
        return muteRamping_ ? muteGain_ : nullptr;
    }

    // ---- inspection (tests, UI) ----
    AssignMode mode() const noexcept { return mode_; }
    AssignMode pendingMode() const noexcept { return pendingMode_; }
    OutAssign assign(int track) const noexcept { return assign_[static_cast<std::size_t>(track & (kTracks - 1))].current; }
    bool fading() const noexcept { return mode_ != pendingMode_ || ! modeFade_.idle() || trackFades_ != 0; }
    // The reference quirk's state: the mode was re-selected during its fade-out, every output stays silent.
    bool stuck() const noexcept { return mode_ == pendingMode_ && modeFade_.idle() && modeFade_.current() == 0.0f; }
    // Where track t is sent this block (bit p = pair p left, bit 4 + p = pair p right; mono adds one side): tests.
    std::uint8_t destinations(int track) const noexcept;

private:
    struct TrackAssign
    {
        OutAssign current {}, pending {}; // the selection playing / selected (research Selector cur / target)
        routing::SelectorFade fade;
    };
    void addTrack(int track, const stages::StereoBlock& b, const float* gain) noexcept;
    bool resolve(OutAssign a, int& pair, int& mono) const noexcept; // false = not routed (master only)
    float* pairFor(int pair) noexcept;                              // the pair's block, cleared on first use
    void updateRouted() noexcept;
    void stepSelectors() noexcept;
    void renderMuteAll() noexcept;

    AssignMode mode_ = AssignMode::MasterPlus6, pendingMode_ = AssignMode::MasterPlus6;
    routing::SelectorFade modeFade_;
    bool quirk_ = false;
    std::array<TrackAssign, kTracks> assign_ {};
    std::uint32_t trackFades_ = 0;     // bit t = track t's assignment is changing or fading
    std::uint32_t routed_ = 0;         // bit t = track t reaches a direct output (or is fading)
    bool dirty_ = true;                // routed_ needs a recount (mode, assignment or presence changed)
    std::uint8_t present_ = 1;         // bit p = pair p exists
    std::uint8_t used_ = 0;            // bit p = pair p received a track this block
    bool muteAll_ = false, muteRamping_ = false, muteSilent_ = false;
    int mutePos_ = kMuteAllFrames;     // 0 … 64: frames into the current mute-all fade (64 = settled)
    const float* tapL_ = nullptr;
    const float* tapR_ = nullptr;
    alignas(16) float pairBuf_[kRouterPairs][2][kBlockFrames] {};
    alignas(16) float modeGain_[kBlockFrames] {};
    alignas(16) float trackGain_[kBlockFrames] {};
    alignas(16) float muteGain_[kBlockFrames] {};
};
} // namespace engine::mixer
