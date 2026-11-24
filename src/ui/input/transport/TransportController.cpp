#include "ui/input/transport/TransportController.h"

#include "ui/shell/ParamAccess.h"
#include "ui/shell/UiState.h"

#include <cmath>
#include <initializer_list>
#include <string_view>

namespace ui::input::transport
{
namespace mt = core::musictime;
using engine::TransportCmd;

TransportController::TransportController(engine::EngineBridge& bridge, ParamAccess* values,
                                         const core::ParamRegistry& params)
    : bridge_(bridge), values_(values), params_(params)
{
    tempo_ = params_.find("transport.tempo");
    nudgeSpec_ = params_.find("transport.nudge");
    preCount_ = params_.find("transport.preCount");
}

void TransportController::post(const TransportCmd& c)
{
    bridge_.post(c);
    posted_ = true;
    postedAt_ = feedBlock_;
}

void TransportController::setParam(const core::ParamSpec* spec, float value)
{
    if (spec == nullptr)
        return;
    if (values_ != nullptr)
    {
        values_->set(*spec, 0, value);
        return;
    }
    if (const auto id = params_.idOf(spec->id))
        bridge_.post(engine::ParamChange {*id, 0, value, 0});
}

int TransportController::preCountBars() const
{
    if (values_ == nullptr || preCount_ == nullptr)
        return 0;
    const long bars = std::lround(values_->get(*preCount_, 0));
    return bars < 0 ? 0 : (bars > 2 ? 2 : static_cast<int>(bars));
}

double TransportController::tempo() const
{
    if (external_)
        return feedBpm_; // SYN-012: the followed clock's tempo, not the project's
    if (values_ != nullptr && tempo_ != nullptr)
        return mt::clampBpm(static_cast<double>(values_->get(*tempo_, 0)));
    return feedBpm_;
}

void TransportController::onPlay()
{
    if (external_)
    {
        // MIDI clock sync: the external device starts the sequencer (Start / Continue); PLAY only arms.
        if (state_ == State::Stopped || state_ == State::Paused)
            playArmed_ = ! playArmed_;
        return;
    }
    switch (state_)
    {
        case State::Stopped:
        {
            const int bars = recArmed_ ? preCountBars() : 0; // SEQ-044: the pre-count runs when recording from stop
            post({TransportCmd::Play, static_cast<double>(bars)});
            state_ = bars > 0 ? State::PreCount : State::Playing;
            break;
        }
        case State::Playing:
            post({TransportCmd::Stop, 0.0}); // PLAY while playing pauses (SEQ-006)
            state_ = State::Paused;
            break;
        case State::Paused:
            post({TransportCmd::Continue, 0.0});
            state_ = State::Playing;
            break;
        case State::PreCount: // PLAY during the count-in cancels it
            post({TransportCmd::Stop, 1.0});
            state_ = State::Stopped;
            break;
    }
}

void TransportController::onStop()
{
    // STOP: back to step 1, tails ring; a second STOP (already stopped) silences everything (SYN-001).
    post({TransportCmd::Stop, state_ == State::Stopped ? 2.0 : 1.0});
    state_ = State::Stopped;
    playArmed_ = false;
}

void TransportController::onRec()
{
    // While SAMPLING/EDIT (sampling.screenOpen) or LIVE SAMPLING (live.mode) is open the REC key is the
    // sampler's (sampling.md "Interactions": the screen's REC), not the sequencer's record arm.
    if (values_ != nullptr)
        for (const auto id : {std::string_view("sampling.screenOpen"), std::string_view("live.mode")})
            if (const auto* spec = params_.find(id); spec != nullptr && values_->get(*spec, 0) >= 0.5f)
                return;
    recArmed_ = ! recArmed_;
}

void TransportController::onTap(double nowMs)
{
    if (external_)
        return; // the clock sets the tempo (bpm-quantize.md: TAP is disabled under an external clock)
    const auto bpm = tap_.tap(nowMs);
    if (! bpm)
        return;
    const double b = mt::clampBpm(*bpm);
    if (values_ != nullptr && tempo_ != nullptr)
        values_->set(*tempo_, 0, static_cast<float>(b));
    else
        bridge_.post(TransportCmd {TransportCmd::SetTempo, b});
}

void TransportController::onNudge(int direction, bool held)
{
    const int dir = direction > 0 ? 1 : (direction < 0 ? -1 : 0);
    if (external_ && held)
        return; // disabled under an external clock (a release still ends a nudge held before)
    int next = nudge_;
    if (held)
        next = dir;
    else if (nudge_ == dir)
        next = 0; // releasing the other side's key keeps the held one
    if (next == nudge_)
        return;
    nudge_ = next;
    setParam(nudgeSpec_, static_cast<float>(nudge_));
}

bool TransportController::handle(const io::ControlEvent& e)
{
    using Kind = io::ControlEvent::Kind;
    const bool press = e.kind == Kind::Press, release = e.kind == Kind::Release;
    switch (e.control)
    {
        case io::Control::Play:
            if (press)
                onPlay();
            return press || release;
        case io::Control::Stop:
            if (press)
                onStop();
            return press || release;
        case io::Control::Rec:
            if (press)
                onRec();
            return press || release;
        case io::Control::ExtTap:
            if (press)
                onTap(e.timeMs);
            return press || release;
        case io::Control::ExtNudgeDown:
        case io::Control::ExtNudgeUp:
            if (press || release)
                onNudge(e.control == io::Control::ExtNudgeUp ? 1 : -1, press);
            return press || release;
        default: return false;
    }
}

void TransportController::sync(const engine::PlayheadFeed& feed)
{
    feedBlock_ = feed.blockIndex;
    feedPpq_ = feed.ppq;
    feedBpm_ = feed.bpm;
    external_ = feed.externalClock;
    clockLost_ = feed.clockLost;
    if (! external_ || feed.playing || feed.preCount)
        playArmed_ = false; // sync off, or the clock started the transport
    if (feed.blockIndex == 0)
        return; // the engine is not running yet
    if (posted_ && feed.blockIndex < postedAt_ + kSettleBlocks)
        return; // our last command may not be in this snapshot yet
    posted_ = false;
    if (feed.preCount)
        state_ = State::PreCount;
    else if (feed.playing)
        state_ = State::Playing;
    else if (state_ == State::Playing || state_ == State::PreCount)
        state_ = feed.ppq != 0.0 ? State::Paused : State::Stopped; // stopped by someone else (project load, MIDI)
    else if (state_ == State::Paused && feed.ppq == 0.0)
        state_ = State::Stopped;
}

void TransportController::publish(UiState& s, double patternStartPpq) const
{
    s.playing = state_ == State::Playing || state_ == State::PreCount;
    s.recArmed = recArmed_;
    s.playArmed = playArmed_ && ! s.playing;
    s.bpm.format("%.1f", tempo());
    s.syncBadge = external_ ? std::string_view("MIDI") : std::string_view {}; // the status row, BPM/QUANTIZE
    s.syncLost = external_ && clockLost_;
    char text[16];
    const auto p = state_ == State::Stopped ? mt::Position {1, 1, 1}
                                            : mt::toPosition(feedPpq_ - (feedPpq_ < 0.0 ? 0.0 : patternStartPpq));
    mt::formatPosition(p, text, static_cast<int>(sizeof(text)));
    s.position.format("%s", text);
}
} // namespace ui::input::transport
