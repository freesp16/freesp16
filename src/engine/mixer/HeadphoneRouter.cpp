#include "engine/mixer/HeadphoneRouter.h"

#include "core/util/GainLaw.h"
#include "engine/mixer/OutputRouterKernels.h"

#include <cstring>

namespace engine::mixer
{
namespace
{
constexpr int kN = kBlockFrames;
constexpr int kGate = HeadphoneRouter::kGateFrames;

float levelGain(int index) noexcept
{
    index = index < 0 ? 0 : (index > 127 ? 127 : index);
    return core::GainLaw::gain(index) * 0.5f; // MIX-041: the fader law × 0.5 (127 = unity)
}

// The selector input of a cue (research SOURCE_PAIRS with router +0x98 = 1: CueMaster and Cue12 are both in2/3).
int sourceOf(Cue c) noexcept
{
    const int i = static_cast<int>(c);
    return i <= static_cast<int>(Cue::Out12) ? 0 : (i <= static_cast<int>(Cue::Out78) ? i - 1 : 4 + i - 5);
}
} // namespace

// ---- Gate: fx_processors.MuteSmoother (N = 44) ----

void HeadphoneRouter::Gate::reset(bool muted) noexcept
{
    muted_ = muted;
    rem_ = 0;
    state_ = muted ? 2 : 0;
    g_ = muted ? 0.0f : 1.0f;
    step_ = muted ? kInv : -kInv;
}

void HeadphoneRouter::Gate::set(bool muted) noexcept
{
    if (muted_ == muted)
        return;
    step_ = muted ? -kInv : kInv;
    muted_ = muted;
    rem_ = kGate - rem_;
    state_ = 1;
}

HeadphoneRouter::Gate::Kind HeadphoneRouter::Gate::fill(float* out, int n) noexcept
{
    if (state_ == 0)
        return Kind::Open;
    if (state_ == 2)
        return Kind::Closed;
    const int total = n;
    int i = 0;
    for (;;)
    {
        if (n == 0)
            return Kind::Buffer;
        if (state_ != 1) // closed after a fade-out that ended in this block
        {
            for (int k = 0; k < n; ++k)
                out[i + k] = 0.0f;
            return Kind::Buffer;
        }
        float g = g_;
        if (n <= rem_)
        {
            for (int k = 0; k < n; ++k)
            {
                out[i + k] = g;
                g = g + step_;
            }
            g_ = g;
            rem_ -= n;
            return Kind::Buffer;
        }
        for (int k = 0; k < rem_; ++k)
        {
            out[i + k] = g;
            g = g + step_;
        }
        i += rem_;
        if (! muted_)
            break;
        n -= rem_;
        state_ = 2;
        rem_ = 0;
        g_ = 0.0f;
    }
    state_ = 0; // open: the rest of the block at gain 1 (the reference leaves those samples untouched)
    rem_ = 0;
    g_ = 1.0f;
    for (; i < total; ++i)
        out[i] = 1.0f;
    return Kind::Buffer;
}

// ---- HeadphoneRouter ----

HeadphoneRouter::HeadphoneRouter() noexcept
{
    (void) core::GainLaw::gain(0); // the lazy table, here (message thread)
    level_.reset(levelGain(100));
    gate_.reset(true);
}

void HeadphoneRouter::prepare(double) noexcept
{
    active_ = pending_ = effectiveCue(requested_, mode_);
    curSrc_ = tgtSrc_ = sourceOf(active_);
    sel_.reset(1.0f);
    gate_.reset(active_ == Cue::Master);
    level_.reset(level_.target());
}

void HeadphoneRouter::setCue(int cueIndex) noexcept
{
    if (cueIndex < 0 || cueIndex >= kCues)
        cueIndex = 0;
    requested_ = static_cast<Cue>(cueIndex);
    retarget(effectiveCue(requested_, mode_));
}

void HeadphoneRouter::setMode(AssignMode m) noexcept
{
    mode_ = m;
    retarget(effectiveCue(requested_, mode_));
}

void HeadphoneRouter::setLevel(float index) noexcept
{
    const int i = static_cast<int>(index < 0.0f ? index - 0.5f : index + 0.5f);
    level_.setTarget(levelGain(i), 0);
}

void HeadphoneRouter::retarget(Cue c) noexcept
{
    pending_ = c; // the selection (research setHeadphoneSource stores the target; process() fades)
    tgtSrc_ = sourceOf(c);
    if (tgtSrc_ == curSrc_)
        active_ = c; // the same selector input (MASTER / OUT 1-2, or a re-selection)
}

void HeadphoneRouter::process(const OutputBuffers& out, const float* const*, const MonitorBuses& mon, float* l,
                              float* r) noexcept
{
    // 0. the selector (research Selector.process): fade the old input out, switch in the first block that starts
    //    with the fade-out over and fade the new one in; a re-selection during the fade-out: see the header (F209).
    if (curSrc_ != tgtSrc_)
    {
        sel_.setTarget(0.0f);
        if (sel_.idle())
        {
            curSrc_ = tgtSrc_;
            active_ = pending_;
            sel_.setTarget(1.0f);
        }
    }
    else if (! quirk_ && sel_.target() == 0.0f)
        sel_.setTarget(1.0f);

    // 1. the selected source (one pair, or one channel duplicated), × the selector fade.
    const float* sl = nullptr;
    const float* sr = nullptr;
    if (curSrc_ < kRouterPairs)
    {
        sl = out.channel(2 * curSrc_);
        sr = out.channel(2 * curSrc_ + 1);
    }
    else
        sl = sr = out.channel(curSrc_ - kRouterPairs);
    const bool ramp = ! sel_.idle();
    if (ramp)
        sel_.fill(sl != nullptr && sr != nullptr ? selGain_ : nullptr, kN);
    if (sl == nullptr || sr == nullptr || (! ramp && sel_.current() == 0.0f))
    {
        std::memset(l, 0, sizeof(float) * kN);
        std::memset(r, 0, sizeof(float) * kN);
    }
    else if (! ramp) // idle at 1
    {
        std::memcpy(l, sl, sizeof(float) * kN);
        std::memcpy(r, sr, sizeof(float) * kN);
    }
    else
    {
        routing::copyGain(l, sl, selGain_, kN);
        routing::copyGain(r, sr, selGain_, kN);
    }

    // 2. the monitor buses at unity, behind the 44-sample gates (closed while the cue is MASTER).
    gate_.set((quirk_ ? pending_ : active_) == Cue::Master);
    const auto kind = gate_.fill(gateGain_, kN);
    if (kind != Gate::Kind::Closed)
    {
        const float* pairs[2][2] = {{mon.auxL, mon.auxR}, {mon.inputL, mon.inputR}};
        for (const auto& p : pairs)
        {
            if (p[0] == nullptr || p[1] == nullptr)
                continue;
            if (kind == Gate::Kind::Buffer)
            {
                routing::addGain(l, p[0], gateGain_, kN);
                routing::addGain(r, p[1], gateGain_, kN);
            }
            else
            {
                routing::add(l, p[0], kN);
                routing::add(r, p[1], kN);
            }
        }
    }

    // 3. the HEADPHONES level (100-sample ramp).
    if (level_.idle())
    {
        const float v = level_.current();
        if (v != 1.0f)
        {
            routing::copyScale(l, l, v, kN);
            routing::copyScale(r, r, v, kN);
        }
    }
    else
    {
        level_.fill(levelGain_, kN);
        routing::multiply(l, levelGain_, kN);
        routing::multiply(r, levelGain_, kN);
    }
}
} // namespace engine::mixer
