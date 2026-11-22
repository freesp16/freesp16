#include "engine/scene/ScenePatternSwitcher.h"

#include <cmath>

namespace engine::scene
{
namespace
{
constexpr double kEps = 1e-9; // quarter notes: double rounding of exact grid positions

bool validIndex(int i) noexcept
{
    return i >= 0 && i < kScenes;
}
} // namespace

void ScenePatternSwitcher::reset() noexcept
{
    state_ = {0, 0, -1, -1, 0.0};
    previous_ = state_;
    applyBpm_ = false;
    switchable_ = true;
    continue_ = false;
    followBpm_ = true;
    masksDirty_ = true;
    unit_ = SwitchUnit::Pattern;
    sceneBpm_.fill(0.0f);
    for (auto& scene : length_)
        for (auto& l : scene)
            l = 16; // the model's default pattern length (SEQ-001)
    for (int s = 0; s < kScenes; ++s)
        for (int p = 0; p < kPatterns; ++p)
            mute_[s][p] = solo_[s][p] = 0;
}

void ScenePatternSwitcher::request(int scene, int pattern, bool applyBpm) noexcept
{
    if (! switchable_ || ! validIndex(scene) || ! validIndex(pattern))
        return; // SCN-023 / SCN-051: ignored while switching is blocked
    if (scene == state_.scene && pattern == state_.pattern)
    {
        cancel(); // the current pattern again (SCN-021); stopped: nothing to do
        return;
    }
    state_.pendingScene = static_cast<std::int8_t>(scene);
    state_.pendingPattern = static_cast<std::int8_t>(pattern);
    applyBpm_ = applyBpm;
}

void ScenePatternSwitcher::cancel() noexcept
{
    state_.pendingScene = state_.pendingPattern = -1;
    applyBpm_ = false;
}

void ScenePatternSwitcher::applyQueue(float value) noexcept
{
    const int v = static_cast<int>(value < 0.0f ? value - 0.5f : value + 0.5f);
    switch (v)
    {
        case kQueueCancel: cancel(); return;
        case kQueueBlock: setSwitchable(false); return;
        case kQueueUnblock: setSwitchable(true); return;
        default: break;
    }
    if (v < 0 || v >= 2 * kQueueApplyBpm)
        return;
    const int target = v & (kQueueApplyBpm - 1);
    request(target / kPatterns, target % kPatterns, (v & kQueueApplyBpm) != 0);
}

void ScenePatternSwitcher::setSwitchable(bool on) noexcept
{
    switchable_ = on;
}

void ScenePatternSwitcher::setUnit(SwitchUnit u) noexcept
{
    unit_ = u;
}

void ScenePatternSwitcher::setContinue(bool on) noexcept
{
    continue_ = on;
}

void ScenePatternSwitcher::setSceneBpm(int scene, float bpm) noexcept
{
    if (validIndex(scene))
        sceneBpm_[static_cast<std::size_t>(scene)] = bpm > 0.0f ? bpm : 0.0f;
}

void ScenePatternSwitcher::setFollowSceneBpm(bool on) noexcept
{
    followBpm_ = on;
}

void ScenePatternSwitcher::setPatternLength(int scene, int pattern, int steps) noexcept
{
    if (! validIndex(scene) || ! validIndex(pattern))
        return;
    length_[scene][pattern] = static_cast<std::uint8_t>(steps < 1 ? 1 : (steps > kMaxSteps ? kMaxSteps : steps));
}

void ScenePatternSwitcher::setPatternMuteSolo(int scene, int pattern, std::uint16_t mute, std::uint16_t solo) noexcept
{
    if (! validIndex(scene) || ! validIndex(pattern))
        return;
    if (mute_[scene][pattern] == mute && solo_[scene][pattern] == solo)
        return;
    mute_[scene][pattern] = mute;
    solo_[scene][pattern] = solo;
    if (scene == state_.scene && pattern == state_.pattern)
        masksDirty_ = true; // an edit of the playing pattern: the mixer hears it at the next block start
}

double ScenePatternSwitcher::nextBoundary(double p0) const noexcept
{
    const double start = state_.patternStartPpq;
    const double lenQ = lengthQuarters(state_.scene, state_.pattern);
    const double unit = unit_ == SwitchUnit::Step ? kQuartersPerStep : (unit_ == SwitchUnit::Bar ? kQuartersPerBar : lenQ);
    double k = std::ceil((p0 - start) / unit - kEps);
    if (k < 0.0)
        k = 0.0;
    const double b = start + k * unit;
    return b > start + lenQ ? start + lenQ : b; // BAR: a pattern shorter than its last bar ends that bar early
}

void ScenePatternSwitcher::switchTo(double at, int frame, SwitchResult& r) noexcept
{
    const int scene = state_.pendingScene, pattern = state_.pendingPattern;
    double& start = state_.patternStartPpq;
    if (continue_)
    {
        // SCN-025 CONTINUE: the step position within the pass, kept modulo the new length.
        double pos = at - start;
        pos = pos > 0.0 ? std::fmod(pos, lengthQuarters(state_.scene, state_.pattern)) : 0.0;
        pos = std::fmod(pos, lengthQuarters(scene, pattern));
        start = at - pos;
    }
    else
        start = at; // RESTART: the new pattern's step 1 sits on the boundary
    r.switched = true;
    r.sceneChanged = scene != state_.scene;
    r.frame = frame;
    if (r.sceneChanged && sceneBpm_[static_cast<std::size_t>(scene)] > 0.0f && (followBpm_ || applyBpm_))
        r.tempo = sceneBpm_[static_cast<std::size_t>(scene)]; // SCN-004
    state_.scene = static_cast<std::uint8_t>(scene);
    state_.pattern = static_cast<std::uint8_t>(pattern);
    state_.pendingScene = state_.pendingPattern = -1;
    applyBpm_ = false;
    masksDirty_ = true; // SCN-012: the new pattern's masks, from the switch frame
}

SwitchResult ScenePatternSwitcher::advance(const BlockContext& ctx, stages::BlockBuffers::Timing& timing) noexcept
{
    previous_ = state_;
    SwitchResult r;
    timing.boundary = {};
    const bool running = ctx.playing && ! timing.preCount;
    const bool pending = state_.pendingPattern >= 0 && switchable_;
    const double p0 = ctx.ppqStart, p1 = ctx.ppqEnd;
    double& start = state_.patternStartPpq;

    if (ctx.startedThisBlock && p0 <= kEps)
        start = 0.0; // PLAY from the top, or the pre-count (PPQ −4 × bars … 0): the pass starts at PPQ 0
    if (running)
    {
        const double lenQ = lengthQuarters(state_.scene, state_.pattern);
        if (p0 >= 0.0 && p0 < start - kEps) // located backwards: step back whole passes, keep the phase
            start -= std::ceil((start - p0) / lenQ - kEps) * lenQ;
        if (p0 >= start + lenQ - kEps) // passes completed before this block (wraps, a shortened pattern, a locate)
            start += std::floor((p0 - start) / lenQ + kEps) * lenQ;
        if (pending)
        {
            const double frames = ctx.frames > 0 ? static_cast<double>(ctx.frames) : static_cast<double>(kBlockFrames);
            const double ppqPerFrame = (p1 - p0) / frames;
            const double b = nextBoundary(p0);
            // In this block iff its frame is < 64 (the same rounding as frameAt; the next block finds it at frame 0).
            if (ppqPerFrame > 0.0 && (b - p0) / ppqPerFrame + 1e-6 < frames)
            {
                const int frame = frameAt(b, p0, ppqPerFrame);
                switchTo(b, frame, r);
                timing.boundary.has = true;
                timing.boundary.frame = static_cast<std::uint8_t>(frame);
                timing.boundary.ppq = b;
            }
        }
    }
    else if (pending)
        switchTo(p0 > 0.0 ? p0 : 0.0, 0, r); // SCN-024: stopped (or counting in) — immediate, at the block start

    if (masksDirty_)
    {
        r.masksChanged = true;
        masksDirty_ = false;
    }
    timing.scene = state_.scene;
    timing.pattern = state_.pattern;
    timing.patternStartPpq = start;
    timing.sceneChanged = r.sceneChanged;
    return r;
}

int ScenePatternSwitcher::stepAt(double ppq) const noexcept
{
    const double x = ppq - state_.patternStartPpq;
    if (x < 0.0)
        return 0;
    const int step = static_cast<int>(x / kQuartersPerStep + kEps);
    return step % currentLength();
}

void ScenePatternSwitcher::publish(PlayheadFeed& feed, double ppq) const noexcept
{
    feed.scene = state_.scene;
    feed.pattern = state_.pattern;
    feed.step = static_cast<std::uint8_t>(stepAt(ppq));
    feed.pendingScene = state_.pendingScene;
    feed.pendingPattern = state_.pendingPattern;
}
} // namespace engine::scene
