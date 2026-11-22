#include "engine/scene/SectionSequencer.h"

#include "core/util/musictime/MusicTime.h"

#include <cmath>

namespace engine::scene
{
namespace
{
constexpr double kEps = 1e-9; // quarter notes: as ScenePatternSwitcher.cpp

int roundToInt(float v) noexcept
{
    return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
}
} // namespace

void SectionSequencer::reset() noexcept
{
    plan_ = nullptr;
    songMode_ = loop_ = active_ = playing_ = false;
    row_ = remaining_ = 0;
    resetCue();
    jumpRow_ = -1;
    jumpRemaining_ = 0;
    passStart_ = lastPpq_ = 0.0;
    rowBegan_ = startEdge_ = passEnded_ = false;
    rowChanges_ = 0;
}

void SectionSequencer::resetCue() noexcept
{
    cueRow_ = 0;
    cueRemaining_ = 0;
}

void SectionSequencer::bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                                  bridge::ParamBindings& bindings) noexcept
{
    if (! resolve)
        return;
    playId_ = resolve(kPlayId);
    loopId_ = resolve(kLoopId);
    startId_ = resolve(kStartId);
    for (const auto id : {playId_, loopId_, startId_})
        bindings.bind(id, bridge::ParamTarget::Sequencer); // Engine::apply offers them to onParam first
}

bool SectionSequencer::onParam(const ParamChange& c) noexcept
{
    const auto id = c.param;
    if (! id.valid())
        return false;
    if (id == playId_)
        setSongMode(c.value >= 0.5f);
    else if (id == loopId_)
        setLoop(c.value >= 0.5f);
    else if (id == startId_)
    {
        if (const int row = roundToInt(c.value); row >= 0)
            startFrom(row);
    }
    else
        return false;
    return true;
}

bool SectionSequencer::refusesSwitch(float queueValue) const noexcept
{
    return engaged() && roundToInt(queueValue) >= 0; // cancel / block / unblock (< 0) still pass
}

int SectionSequencer::repeatsOf(int row) const noexcept
{
    const int r = plan_->rows[static_cast<std::size_t>(row)].repeats;
    return r < 1 ? 1 : r;
}

void SectionSequencer::install(const ArrangementPlan* plan) noexcept
{
    if (plan_ != nullptr && plan_->status != nullptr && (plan == nullptr || plan->status != plan_->status))
        plan_->status->row.store(-1, std::memory_order_relaxed); // the old plan's reader: no song any more
    plan_ = plan;
    if (plan == nullptr || plan->count == 0)
    {
        active_ = false;
        jumpRow_ = -1;
        resetCue();
        return;
    }
    const int count = plan->count > core::arrangement::kMaxRows ? core::arrangement::kMaxRows : plan->count;
    if (cueRow_ >= count)
        resetCue();
    if (cueRemaining_ > repeatsOf(cueRow_))
        cueRemaining_ = 0;
    if (jumpRow_ >= count)
        jumpRow_ = -1;
    if (! active_)
        return;
    if (row_ >= count)
    {
        row_ = count - 1; // the playing row was deleted: the song ends (or loops) at the next pattern end
        remaining_ = 1;
    }
    else if (remaining_ > repeatsOf(row_))
        remaining_ = repeatsOf(row_);
}

void SectionSequencer::setSongMode(bool on) noexcept
{
    if (on == songMode_)
        return;
    songMode_ = on;
    if (! on)
    {
        active_ = false; // the current pattern keeps playing (it loops)
        jumpRow_ = -1;
    }
}

void SectionSequencer::setLoop(bool on) noexcept
{
    loop_ = on;
}

void SectionSequencer::startFrom(int row) noexcept
{
    row = row < 0 ? 0 : row;
    if (plan_ != nullptr && plan_->count > 0)
        row = row >= plan_->count ? plan_->count - 1 : row; // else: kept for its posted plan (install clamps)
    if (active_ && playing_)
    {
        jumpRow_ = row; // at the next pattern end (ARR-006)
        jumpRemaining_ = 0;
        return;
    }
    active_ = false; // stopped or paused: PLAY / CONTINUE starts from it
    cueRow_ = row;
    cueRemaining_ = 0;
}

void SectionSequencer::locate16th(std::int64_t pos) noexcept
{
    if (plan_ == nullptr || plan_->count == 0)
        return;
    const std::int64_t total = plan_->startPpq16[plan_->count];
    pos = pos < 0 ? 0 : (pos >= total ? (total > 0 ? total - 1 : 0) : pos);
    const int row = total > 0 ? plan_->rowAt(pos) : 0;
    const std::int64_t len = plan_->passLength16(row);
    const std::int64_t done = len > 0 ? (pos - plan_->startPpq16[static_cast<std::size_t>(row)]) / len : 0;
    int remaining = repeatsOf(row) - static_cast<int>(done);
    remaining = remaining < 1 ? 1 : remaining;
    if (active_ && playing_)
    {
        jumpRow_ = row;
        jumpRemaining_ = remaining;
        return;
    }
    active_ = false;
    cueRow_ = row;
    cueRemaining_ = remaining;
}

void SectionSequencer::cue(ScenePatternSwitcher& sw) noexcept
{
    const auto& s = plan_->rows[static_cast<std::size_t>(cueRow_)];
    const SwitchState& st = sw.state();
    const bool current = st.scene == s.scene && st.pattern == s.pattern;
    const bool pending = st.pendingScene == s.scene && st.pendingPattern == s.pattern;
    if (current && st.pendingPattern >= 0)
        sw.cancel(); // a request made before song mode came on
    else if (! current && ! pending)
    {
        sw.request(s.scene, s.pattern); // stopped: immediate (SCN-024)
        rowBegan_ = true;               // its tempo (the scene BPM does not govern in song mode)
    }
    if (s.bpm > 0.0f)
        rowBegan_ = true; // a fixed BPM rules while the row is cued (afterSwitch sends it only when it differs)
}

void SectionSequencer::enterRow(ScenePatternSwitcher& sw, int row, int remaining, double at) noexcept
{
    const int repeats = repeatsOf(row);
    row_ = row;
    remaining_ = remaining > 0 && remaining < repeats ? remaining : repeats;
    passStart_ = at;
    rowBegan_ = true;
    ++rowChanges_;
    const auto& s = plan_->rows[static_cast<std::size_t>(row)];
    sw.request(s.scene, s.pattern); // lands on `at` (the switcher's boundary of this block); the current one = cancel
}

bool SectionSequencer::needsSwitch(const ScenePatternSwitcher& sw, int row) const noexcept
{
    const auto& s = plan_->rows[static_cast<std::size_t>(row)];
    return sw.state().scene != s.scene || sw.state().pattern != s.pattern;
}

bool SectionSequencer::advance(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                               ScenePatternSwitcher& sw) noexcept
{
    rowBegan_ = startEdge_ = passEnded_ = false;
    lastPpq_ = ctx.ppqEnd;
    playing_ = ctx.playing;
    if (! engaged())
    {
        active_ = false;
        return false;
    }
    if (! ctx.playing)
    {
        if (active_ && ctx.ppqStart > kEps)
            return false; // paused: the song holds its row and pass (CONTINUE resumes them)
        if (active_)
        {
            active_ = false; // stopped at the top: PLAY starts from row 1 again
            jumpRow_ = -1;
            resetCue();
        }
        cue(sw);
        return false;
    }
    if (! active_ && ctx.startedThisBlock)
    {
        // PLAY (from the top, after a pre-count) or CONTINUE: the cued row, which the stopped switcher already plays.
        active_ = true;
        startEdge_ = true;
        row_ = cueRow_;
        remaining_ = cueRemaining_ > 0 ? cueRemaining_ : repeatsOf(cueRow_);
        passStart_ = ctx.ppqStart > 0.0 ? ctx.ppqStart : 0.0; // provisional: afterSwitch reads the switcher's
        rowBegan_ = true;
        ++rowChanges_;
        resetCue();
        const auto& s = plan_->rows[static_cast<std::size_t>(row_)];
        if (needsSwitch(sw, row_))
            sw.request(s.scene, s.pattern); // not cued yet (posted with PLAY): the start boundary takes it
    }
    if (! active_ && sw.state().pendingPattern >= 0)
        sw.cancel(); // song mode came on while playing: a request made before it does not land first
    int frame = 0;
    double at = 0.0;
    if (! PatternChainPlayer::patternEndInBlock(ctx, timing.preCount, sw, frame, at))
        return false;
    if (! active_)
    {
        // Song mode came on while playing: the cued row from this pattern end.
        passEnded_ = true;
        active_ = true;
        enterRow(sw, cueRow_, cueRemaining_, at);
        resetCue();
        return false;
    }
    return onPatternEnd(sw, at);
}

bool SectionSequencer::onPatternEnd(ScenePatternSwitcher& sw, double at) noexcept
{
    passEnded_ = true;
    if (! active_ || plan_ == nullptr || plan_->count == 0)
        return false;
    --remaining_;
    if (jumpRow_ >= 0)
    {
        const int row = jumpRow_, remaining = jumpRemaining_;
        jumpRow_ = -1;
        enterRow(sw, row, remaining, at);
        return false;
    }
    if (remaining_ > 0)
    {
        passStart_ = at; // the next pass of the same row: the pattern wraps on `at`
        if (needsSwitch(sw, row_))
            sw.request(plan_->rows[static_cast<std::size_t>(row_)].scene,
                       plan_->rows[static_cast<std::size_t>(row_)].pattern); // the row was edited while playing
        return false;
    }
    int next = row_ + 1;
    if (next >= plan_->count)
    {
        if (! loop_)
        {
            active_ = false; // ARR-005 stop (default): the caller stops the transport
            jumpRow_ = -1;
            resetCue();
            return true;
        }
        next = 0; // ARR-005 loop
    }
    if (! sw.switchable() && needsSwitch(sw, next))
    {
        remaining_ = 1; // SCN-023: switching is blocked (SAMPLING / EDIT): the row holds, one pass at a time
        passStart_ = at;
        return false;
    }
    enterRow(sw, next, 0, at);
    return false;
}

bool SectionSequencer::settle(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                              double tempoBefore, double tempoNow, double& tempo) noexcept
{
    (void) ctx;
    if (startEdge_ && ! passEnded_)
        passStart_ = timing.patternStartPpq; // the pass the switcher started on the start edge
    publish();
    if (! rowBegan_ || ! engaged())
        return false;
    const float bpm = plan_->rows[static_cast<std::size_t>(active_ ? row_ : cueRow_)].bpm;
    // ARR-002 / ARR-006: a fixed BPM steps; "--" keeps the tempo in force before the row (also against a scene BPM
    // the switcher applied at this switch, SCN-004).
    const double target = bpm > 0.0f ? core::musictime::clampBpm(static_cast<double>(bpm)) : tempoBefore;
    if (! (target > 0.0) || target == tempoNow)
        return false;
    tempo = target;
    return true;
}

RowState SectionSequencer::playing() const noexcept
{
    RowState s;
    s.cued = static_cast<std::int16_t>(cueRow_);
    if (active_ && plan_ != nullptr && plan_->count > 0)
    {
        s.row = static_cast<std::int16_t>(row_);
        s.remaining = static_cast<std::uint8_t>(remaining_ < 0 ? 0 : remaining_);
        s.repeats = static_cast<std::uint8_t>(repeatsOf(row_));
    }
    return s;
}

std::int64_t SectionSequencer::songPositionAt(double ppq) const noexcept
{
    if (plan_ == nullptr || plan_->count == 0)
        return 0;
    if (! active_)
    {
        const int repeats = repeatsOf(cueRow_);
        const int done = cueRemaining_ > 0 ? repeats - cueRemaining_ : 0;
        return plan_->startPpq16[static_cast<std::size_t>(cueRow_)] + done * plan_->passLength16(cueRow_);
    }
    const int done = repeatsOf(row_) - (remaining_ < 1 ? 1 : remaining_);
    const std::int64_t base = plan_->startPpq16[static_cast<std::size_t>(row_)] + done * plan_->passLength16(row_);
    // Sixteenths into the pass (negative just before a pass end inside this block: the previous pass's tail).
    const std::int64_t into = static_cast<std::int64_t>(std::floor((ppq - passStart_) * 4.0 + 1e-6));
    const std::int64_t pos = base + into;
    const std::int64_t last = plan_->startPpq16[plan_->count] - 1;
    return pos < 0 ? 0 : (pos > last ? last : pos);
}

void SectionSequencer::publish() const noexcept
{
    if (plan_ == nullptr || plan_->status == nullptr)
        return;
    auto& st = *plan_->status;
    const bool on = active_ && engaged();
    st.row.store(on ? row_ : -1, std::memory_order_relaxed);
    st.remaining.store(on ? remaining_ : 0, std::memory_order_relaxed);
    st.position16.store(static_cast<std::int32_t>(songPositionAt(lastPpq_)), std::memory_order_relaxed);
    st.cued.store(cueRow_, std::memory_order_relaxed);
}
} // namespace engine::scene
