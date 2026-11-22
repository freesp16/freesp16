#include "engine/sequencer/Sequencer.h"

#include "engine/scene/ScenePatternSwitcher.h"

#include <cmath>

namespace engine::sequencer
{
namespace
{
// Membership tolerance in PPQ: constant, so consecutive blocks ([p0 − e, p1 − e)) still tile the timeline exactly.
// It moves an event that lies within 2·10⁻⁵ frames below a block start (accumulated PPQ rounding) into that block.
constexpr double kEpsPpq = 1e-9;
constexpr int kMaxGridPerSegment = 16; // ≥ (block + early + late window) / step at any tempo up to 999 BPM

int floorDiv(double x) noexcept { return static_cast<int>(std::floor(x)); }
long long floorDiv(long long a, long long b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int wrapStep(int n, int len) noexcept
{
    const int s = n % len;
    return s < 0 ? s + len : s;
}
} // namespace

Sequencer::Sequencer() noexcept = default;

void Sequencer::clearSuppress() noexcept
{
    suppress_.fill(0);
    suppressNext_.fill(0);
    supBase_ = 0;
}

void Sequencer::advancePass(long long pass) noexcept
{
    if (pass == supBase_)
        return;
    if (pass == supBase_ + 1)
        suppress_ = suppressNext_;
    else
        suppress_.fill(0);
    suppressNext_.fill(0);
    supBase_ = pass;
}

void Sequencer::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    clearSuppress();
    hasPrev_ = false;
    startRequest_ = false;
}

void Sequencer::setSwing(float swing01) noexcept
{
    const double s = swing01 < 0.0f ? 0.0 : (swing01 > 0.5f ? 0.5 : static_cast<double>(swing01));
    swing_ = s * kStepPpq;
}

void Sequencer::setTrackOffset(int scene, int track, std::int8_t offset24) noexcept
{
    trackOffset_[static_cast<std::size_t>(scene & 15)][static_cast<std::size_t>(track & 15)] =
        static_cast<std::int8_t>(offset24 < -24 ? -24 : (offset24 > 24 ? 24 : offset24));
}

void Sequencer::setRecordArmed(bool armed) noexcept
{
    if (armed_ != armed)
        clearSuppress();
    armed_ = armed;
}

void Sequencer::setErase(int track, bool held) noexcept
{
    const auto bit = static_cast<std::uint16_t>(1u << (track & 15));
    erase_ = static_cast<std::uint16_t>(held ? (erase_ | bit) : (erase_ & ~bit));
}

void Sequencer::push(EngineEvent::Kind kind, int track, int step, int value, int offset24) noexcept
{
    if (feeds_ == nullptr)
        return;
    EngineEvent e;
    e.kind = kind;
    e.track = static_cast<std::uint8_t>(track & 15);
    e.step = static_cast<std::uint8_t>(step & 63);
    e.value = static_cast<std::int16_t>(value);
    e.offset24 = static_cast<std::int8_t>(offset24);
    feeds_->pushEvent(e); // a full queue counts the drop (EngineFeeds::droppedEvents)
}

void Sequencer::collect(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                        trigger::TriggerRouter& router) noexcept
{
    pushed_ = 0;
    const Where now = follow(ctx, timing);
    // A new run (another pattern, a locate) drops the "recorded this pass" bits of the one that was recording; a start
    // edge keeps them (stopping cleared them; PAD STARTS SEQ set step 1's while stopped).
    if (hasPrev_ && (now.scene != cur_.scene || now.pattern != cur_.pattern ||
                     (now.first && ! ctx.startedThisBlock && now.start != cur_.start)))
        clearSuppress();
    prev_ = cur_;
    const bool hadPrev = hasPrev_;
    cur_ = now;
    hasPrev_ = true;

    const double p0 = ctx.ppqStart, p1 = ctx.ppqEnd;
    const int frames = ctx.frames > 0 ? ctx.frames : kBlockFrames;
    playing_ = ctx.playing && p1 > p0;
    preCount_ = timing.preCount;
    p0_ = p0;
    ppf_ = playing_ ? (p1 - p0) / frames : ctx.bpm / (60.0 * sampleRate_);
    boundary_ = false;
    if (! playing_)
    {
        if (! ctx.playing && ! preCount_)
            clearSuppress(); // stop ends the pass (the count-in keeps a hit recorded onto step 1)
        return;
    }

    if (const auto* pd = patterns_[static_cast<std::size_t>(cur_.scene)][static_cast<std::size_t>(cur_.pattern)])
    {
        const int len = pd->length > 0 ? pd->length : 1;
        const int n = floorDiv((p0 - cur_.start + kEpsPpq) / kStepPpq);
        playStep_ = n >= 0 ? n % len : (cur_.first ? 0 : wrapStep(n, len));
    }
    else
        playStep_ = 0;

    if (preCount_)
        return; // the transport plays the count-in, no pattern (SEQ-044)

    const auto& b = timing.boundary;
    if (b.has && hadPrev && b.ppq > p0 && b.ppq < p1)
    {
        // Old pattern: [p0, B) and frames [0, boundary.frame); new pattern: [B, p1), from boundary.frame on.
        boundary_ = true;
        boundaryFrame_ = b.frame;
        emit(prev_, p0, b.ppq, false, router, b.frame);
        emit(cur_, b.ppq, p1, true, router, kBlockFrames);
    }
    else
        emit(cur_, p0, p1, true, router, kBlockFrames);
}

// The switcher's pass start moves forward by whole passes at every wrap (ScenePatternSwitcher.h, boundary contract):
// the same run, one pass number further per pass. Anything else — another scene or pattern, a start edge, a locate
// backwards, a pass start that is not whole passes later (a length edit the switcher saw first) — begins a new run.
Sequencer::Where Sequencer::follow(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing) const noexcept
{
    Where w {timing.scene & 15, timing.pattern & 15, timing.patternStartPpq, 0, true};
    if (! hasPrev_ || w.scene != cur_.scene || w.pattern != cur_.pattern || ctx.startedThisBlock)
        return w;
    if (w.start == cur_.start)
    {
        w.pass = cur_.pass;
        w.first = cur_.first;
        return w;
    }
    const PatternData* pd = patterns_[static_cast<std::size_t>(w.scene)][static_cast<std::size_t>(w.pattern)];
    const double passPpq = (pd != nullptr && pd->length > 0 ? pd->length : 16) * kStepPpq;
    const double passes = (w.start - cur_.start) / passPpq;
    const double whole = std::floor(passes + 0.5);
    if (whole >= 1.0 && std::fabs(passes - whole) < 1e-6)
    {
        w.pass = cur_.pass + static_cast<long long>(whole);
        w.first = false;
    }
    return w;
}

void Sequencer::emit(const Where& w, double a, double b, bool current, trigger::TriggerRouter& router,
                     int frameEnd) noexcept
{
    const PatternData* pd = patterns_[static_cast<std::size_t>(w.scene)][static_cast<std::size_t>(w.pattern)];
    if (pd == nullptr)
        return;
    const int len = pd->length > 0 ? pd->length : 1;
    const double lo = a - kEpsPpq, hi = b - kEpsPpq;
    // Grid steps that can land in [lo, hi): a step fires at most 1 step early (offset −24) and swing + 1 step late.
    int nLo = static_cast<int>(std::ceil((lo - w.start - swing_ - kStepPpq) / kStepPpq));
    int nHi = static_cast<int>(std::ceil((hi - w.start + kStepPpq) / kStepPpq)); // exclusive
    const int nMin = w.first ? 0 : -len; // nothing fires before the run's first pass; else the previous pass's tail
    if (nLo < nMin)
        nLo = nMin;
    if (nHi - nLo > kMaxGridPerSegment)
        nHi = nLo + kMaxGridPerSegment;
    if (current)
        advancePass(w.pass + floorDiv(static_cast<long long>(nLo), len)); // the oldest grid pass that can still fire
    const double p0 = p0_, ppf = ppf_;
    const double offsetPpq = kStepPpq / 24.0;
    const auto& offsets = trackOffset_[static_cast<std::size_t>(w.scene)];
    std::uint64_t noBits = 0;

    for (int n = nLo; n < nHi; ++n)
    {
        const int s = wrapStep(n, len);
        const double grid = w.start + n * kStepPpq + ((s & 1) != 0 ? swing_ : 0.0);
        const auto* row = pd->cells[s];
        for (int t = 0; t < kTracks; ++t)
        {
            if ((pd->triggerMask[t] >> s & 1u) == 0)
                continue;
            const StepCell& c = row[t];
            const int off = hasOffsetLock(*pd, c) ? c.offset24 : offsets[static_cast<std::size_t>(t)];
            const double at = grid + off * offsetPpq;
            if (at < lo || at >= hi)
                continue;
            if (current)
            {
                const long long pass = w.pass + floorDiv(static_cast<long long>(n), len);
                auto& rec = pass == supBase_       ? suppress_[static_cast<std::size_t>(t)]
                            : pass == supBase_ + 1 ? suppressNext_[static_cast<std::size_t>(t)]
                                                   : noBits;
                if ((rec >> s & 1u) != 0)
                {
                    rec &= ~(std::uint64_t {1} << s); // the live hit already sounded this pass
                    continue;
                }
                if ((erase_ >> t & 1u) != 0)
                {
                    push(EngineEvent::Kind::ErasedStep, t, s, 0, 0);
                    continue;
                }
            }
            const int frame = scene::frameAt(at, p0, ppf); // the switcher's rounding (boundary contract)
            if (frame >= frameEnd)
                continue; // the old pattern's events stop before the boundary frame
            trigger::TriggerEvent e;
            e.track = static_cast<std::uint8_t>(t);
            e.velocity = c.velocity;
            e.semitones = 0;
            e.kind = c.kind;
            e.source = trigger::TriggerSource::Sequencer;
            e.frame = static_cast<std::uint8_t>(frame);
            e.ducks = c.kind != trigger::TriggerKind::LockOnly;
            e.locks = c.lockIndex < pd->locks.size() ? &pd->locks[c.lockIndex] : nullptr;
            if (router.push(e))
                ++pushed_;
            else
                ++routerDropped_;
        }
    }
}

void Sequencer::onLiveHit(const trigger::TriggerEvent& e) noexcept
{
    if (! armed_ || e.kind != trigger::TriggerKind::Full)
        return;
    if (e.source != trigger::TriggerSource::Pad && e.source != trigger::TriggerSource::Midi)
        return; // SEQ-016: only pad and MIDI hits are recorded
    const int track = e.track & 15;
    if (preCount_)
    {
        // SEQ-044 (our choice): a hit late in the count-in (closer to step 1 than to the count-in's last grid point) is
        // recorded onto step 1 of the first pass, which starts at PPQ 0.
        const Where first {cur_.scene, cur_.pattern, 0.0, 0, true};
        record(track, e.velocity, p0_ + e.frame * ppf_, first, true);
        return;
    }
    if (! playing_)
    {
        if (! padStarts_)
            return;
        // SEQ-047: the first hit while stopped starts the transport and is recorded on step 1 (it sounded now, so
        // step 1 must not sound again when the pass starts).
        startRequest_ = true;
        suppress_[static_cast<std::size_t>(track)] |= 1u;
        push(EngineEvent::Kind::RecordedHit, track, 0, e.velocity, 0);
        return;
    }
    const double hit = p0_ + e.frame * ppf_;
    const bool before = boundary_ && e.frame < boundaryFrame_; // recorded into the old pattern's grid
    record(track, e.velocity, hit, before ? prev_ : cur_, ! before);
}

void Sequencer::record(int track, int velocity, double hitPpq, const Where& w, bool current) noexcept
{
    const PatternData* pd = patterns_[static_cast<std::size_t>(w.scene)][static_cast<std::size_t>(w.pattern)];
    const int len = pd != nullptr && pd->length > 0 ? pd->length : 16;
    const double steps = (hitPpq - w.start) / kStepPpq;
    // Before the pattern's first step (the count-in's end, the sub-frame before a RESTART boundary): quantised as the
    // end of a "pass −1", kept only when it lands on step 1 (wrapped); up to one step early.
    const bool early = steps < 0.0;
    if (early && steps < -1.0)
        return;
    const double passBase = early ? -static_cast<double>(len) : std::floor(steps / len) * len;
    const double pos = steps - passBase; // 0 ≤ pos < len
    const double swingSteps = swing_ / kStepPpq;
    const auto q = core::sequencer::quantizeHit(pos, len, quantize_, swingSteps);
    if (early && ! q.wrapped)
        return; // closer to the count-in than to step 1: nothing to record into

    // When does the stored trigger play? (in steps from this pass's start; a wrapped one in the next pass)
    const bool offsetLocked = q.offset24 != 0; // recordHit writes an OFF SET lock only for a non-zero offset
    const auto& offsets = trackOffset_[static_cast<std::size_t>(w.scene)];
    const int off = offsetLocked ? q.offset24 : offsets[static_cast<std::size_t>(track)];
    const double plays = (q.wrapped ? len : 0) + q.step + ((q.step & 1) != 0 ? swingSteps : 0.0) + off / 24.0;
    if (current && plays > pos + 1e-9 && ! (early && plays < len)) // (early: a negative offset never plays in pass 0)
    {
        const long long n = static_cast<long long>(passBase) + q.step + (q.wrapped ? len : 0); // its grid index
        const long long pass = w.pass + floorDiv(n, len);
        const auto bit = std::uint64_t {1} << q.step;
        if (pass == supBase_)
            suppress_[static_cast<std::size_t>(track)] |= bit;
        else if (pass == supBase_ + 1)
            suppressNext_[static_cast<std::size_t>(track)] |= bit;
    }
    push(EngineEvent::Kind::RecordedHit, track, q.step, velocity, q.offset24);
}
} // namespace engine::sequencer
