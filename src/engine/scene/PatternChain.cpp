#include "engine/scene/PatternChain.h"

#include <cmath>

namespace engine::scene
{
namespace
{
constexpr double kEps = 1e-9; // as ScenePatternSwitcher.cpp: double rounding of exact grid positions
} // namespace

void PatternChainPlayer::reset() noexcept
{
    plan_ = nullptr;
    index_ = -1;
    active_ = suspended_ = false;
    advances_ = 0;
}

void PatternChainPlayer::install(const ChainPlan* plan, const ScenePatternSwitcher& switcher) noexcept
{
    plan_ = plan;
    index_ = -1;
    active_ = plan != nullptr && plan->length > 0 && plan->scene == switcher.state().scene;
    if (! active_)
        return;
    const int n = plan->length > core::chain::kMaxChain ? core::chain::kMaxChain : plan->length;
    for (int i = 0; i < n; ++i)
        if (plan->patterns[static_cast<std::size_t>(i)] == switcher.state().pattern)
        {
            index_ = i; // continue from the current pattern's first entry
            break;
        }
}

bool PatternChainPlayer::patternEndInBlock(const BlockContext& ctx, bool preCount, const ScenePatternSwitcher& sw,
                                           int& frame, double& ppq) noexcept
{
    frame = 0;
    ppq = 0.0;
    if (! ctx.playing || preCount)
        return false; // stopped / counting in: no pattern plays, so none ends
    const double p0 = ctx.ppqStart, p1 = ctx.ppqEnd;
    if (ctx.startedThisBlock && p0 <= kEps)
        return false; // PLAY from the top: the first pass starts here
    const double lenQ = static_cast<double>(sw.currentLength()) * kQuartersPerStep;
    if (! (lenQ > 0.0))
        return false;
    // The pass that contains p0 (ScenePatternSwitcher::advance's normalisation, before it runs this block).
    double start = sw.state().patternStartPpq;
    if (p0 >= 0.0 && p0 < start - kEps)
        start -= std::ceil((start - p0) / lenQ - kEps) * lenQ;
    if (p0 >= start + lenQ - kEps)
        start += std::floor((p0 - start) / lenQ + kEps) * lenQ;
    // The switcher's next boundary is p0 itself when p0 sits on the pass start within its unit's tolerance
    // (k = ceil((p0 − start) / unit − ε) = 0): the previous block ended exactly on the pattern end.
    const SwitchUnit u = sw.unit();
    const double unit = u == SwitchUnit::Step ? kQuartersPerStep : (u == SwitchUnit::Bar ? kQuartersPerBar : lenQ);
    const double b = (! ctx.startedThisBlock && p0 - start <= kEps * unit) ? start : start + lenQ;
    const double frames = ctx.frames > 0 ? static_cast<double>(ctx.frames) : static_cast<double>(kBlockFrames);
    const double ppqPerFrame = (p1 - p0) / frames;
    if (! (ppqPerFrame > 0.0) || (b - p0) / ppqPerFrame + 1e-6 >= frames)
        return false; // the next block (frame 0) or later
    frame = frameAt(b, p0, ppqPerFrame);
    ppq = b;
    return true;
}

void PatternChainPlayer::onPatternEnd(ScenePatternSwitcher& sw) noexcept
{
    if (! active() || suspended_)
        return;
    const ChainPlan& plan = *plan_;
    const SwitchState& st = sw.state();
    if (st.scene != plan.scene || st.pendingPattern >= 0)
    {
        active_ = false; // another scene is current, or a user switch waits for this boundary: the chain ends
        return;
    }
    if (! sw.switchable())
        return; // SCN-023: switching blocked (SAMPLING / EDIT): hold the current entry
    const int n = plan.length > core::chain::kMaxChain ? core::chain::kMaxChain : plan.length;
    int next = 0;
    if (index_ >= 0)
    {
        if (plan.patterns[static_cast<std::size_t>(index_)] != st.pattern)
        {
            active_ = false; // a switch the chain did not ask for happened (SCN-030: loops until changed)
            return;
        }
        next = index_ + 1 < n ? index_ + 1 : 0;
    }
    index_ = next;
    ++advances_;
    const int target = plan.patterns[static_cast<std::size_t>(next)] & 15;
    if (target != st.pattern)
        sw.request(st.scene, target); // lands on this block's boundary (see the header)
}

void PatternChainPlayer::process(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                                 ScenePatternSwitcher& sw, const ChainPlan* plan) noexcept
{
    if (plan != plan_)
        install(plan, sw);
    if (! active_ || suspended_)
        return;
    int frame = 0;
    if (patternEndInBlock(ctx, timing.preCount, sw, frame))
        onPatternEnd(sw);
}
} // namespace engine::scene
