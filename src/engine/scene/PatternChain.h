// Pattern chain playback on the audio thread (spec 02-features/scenes-and-patterns.md SCN-030/031). Runs in step 5
// right before the scene switcher (engine/scene/ScenePatternSwitcher.h):
//
//   process(ctx, timing, switcher, plan)   plan = the installed ChainPlan (SwapSlot::ProjectConfig 0/0, or null)
//     └─ when the current pattern's pass ends inside this block (patternEndInBlock, the switcher's boundary contract)
//        onPatternEnd(switcher): request the chain's next entry → the switcher lands it on that very boundary
//
// Exactly at the pattern end, whatever PATTERN QUANTIZE is: the request is made in the block that contains the pass
// end B, before the switcher's advance(), so its next boundary ≥ p0 is B for every unit (STEP / BAR boundaries are
// ≤ 1 per block below 600 BPM and B is one of them; PATTERN = the pass end). Hence no extra repeat (the reference quirk
// "a chained pattern repeats once before advancing" is not reproduced, SCN-031). A repeated entry (2→2) requests
// nothing: the pattern simply wraps.
//
// Rules (our choices): the chain plays only while its scene is current and the transport runs (not in the pre-count); a
// newly installed plan continues from the current pattern's first entry, or enters at entry 1 at the next pattern end
// when the current pattern is not part of it; a switch the chain did not ask for (a manual selection, a pending user
// request at the pattern end) ends the chain ("loops until changed", SCN-030); while switching is blocked (SCN-023) the
// chain holds its entry; setSuspended(true) (song mode) stops it requesting.
//
// Real-time: noexcept, no allocation, O(1) per block — inactive ≈ 3 compares; active: one boundary computation (a few
// double ops, one floor/ceil) per block, one request() per pattern end (bench: tests/bench/bench_edit.cpp).
#pragma once

#include "core/util/chain/ChainPlan.h"
#include "engine/EngineApi.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"

namespace engine::scene
{
using ChainPlan = core::chain::ChainPlan;
inline constexpr SwapSlot kChainSlot {SwapSlot::ProjectConfig, core::chain::kSwapScene, core::chain::kSwapIndex};
inline void destroyChainPlan(const void* plan) noexcept { core::chain::destroyChainPlan(plan); }

class PatternChainPlayer
{
public:
    void reset() noexcept;
    void setSuspended(bool on) noexcept { suspended_ = on; } // song mode plays

    // Step 5, before ScenePatternSwitcher::process. `timing.preCount` is the transport's (step 3).
    void process(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, ScenePatternSwitcher& switcher,
                 const ChainPlan* plan) noexcept;
    // The current pattern ends at this block's boundary: queue the chain's next entry (SCN-031).
    void onPatternEnd(ScenePatternSwitcher& switcher) noexcept;

    // True when the switcher's current pass ends in the block [ctx.ppqStart, ctx.ppqEnd) (same arithmetic as
    // ScenePatternSwitcher::advance: pass normalisation, half-open block, frameAt rounding); `frame` = its frame.
    static bool patternEndInBlock(const BlockContext& ctx, bool preCount, const ScenePatternSwitcher& switcher,
                                  int& frame) noexcept
    {
        double ppq = 0.0;
        return patternEndInBlock(ctx, preCount, switcher, frame, ppq);
    }
    // … and `ppq` = the pass end B itself (song rows start there; the switcher lands a request made now on B).
    static bool patternEndInBlock(const BlockContext& ctx, bool preCount, const ScenePatternSwitcher& switcher,
                                  int& frame, double& ppq) noexcept;

    bool active() const noexcept { return active_ && plan_ != nullptr; }
    int index() const noexcept { return index_; } // the entry now playing, −1 = not entered yet
    std::uint32_t advances() const noexcept { return advances_; } // pattern ends handled (diagnostics, tests)

private:
    void install(const ChainPlan* plan, const ScenePatternSwitcher& switcher) noexcept;

    const ChainPlan* plan_ = nullptr; // the installed plan last seen (owned by the swap table)
    int index_ = -1;
    bool active_ = false, suspended_ = false;
    std::uint32_t advances_ = 0;
};
} // namespace engine::scene
