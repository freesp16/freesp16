#include "engine/midi/MidiEngine.h"

#include "engine/ribbon/RibbonEngine.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/scene/SectionSequencer.h"
#include "engine/sequencer/perf/PerfStage.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/transport/TransportStage.h"
#include "engine/trigger/TriggerRouter.h"

#include <algorithm>
#include <cmath>

namespace engine::midi
{
namespace
{
using F = MidiField;
constexpr int kSteerFrames = 2048;   // the phase error is steered out over ~46 ms of the clock's tempo
constexpr double kMaxSteer = 0.05;   // ±5 % rate while steering
constexpr std::uint8_t kNotOurs = static_cast<std::uint8_t>(MidiField::Count);
constexpr int kPitchLockSlot = 4; // PITCH (core::ParamIndex 4, engine::voice::lockslot::Pitch), semitones
constexpr double kRemoteSelectWait = 1.0; // s: how long a CC 0 waits for its CC 32 (our choice; manual p.88 is silent)

inline int toInt(float v) noexcept { return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f); }
inline int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

trigger::TriggerEvent hit(int track, int velocity, int semis, trigger::TriggerKind kind, int frame) noexcept
{
    trigger::TriggerEvent e {};
    e.track = static_cast<std::uint8_t>(track & 15);
    e.velocity = static_cast<std::uint8_t>(velocity);
    e.semitones = static_cast<std::int8_t>(semis);
    e.kind = kind;
    e.source = trigger::TriggerSource::Midi;
    e.frame = static_cast<std::uint8_t>(frame < 0 ? 0 : (frame > kBlockFrames - 1 ? kBlockFrames - 1 : frame));
    e.ducks = false;
    e.locks = nullptr;
    return e;
}
} // namespace

MidiEngine::MidiEngine() noexcept
{
    field_.fill(kNotOurs);
    heldNote_.fill(-1);
    out_.attach(&hub_);
    hubClaimed_ = core::midi::claimLiveHub(&hub_);
}

MidiEngine::~MidiEngine()
{
    if (hubClaimed_)
        core::midi::releaseLiveHub(&hub_);
}

void MidiEngine::bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                            bridge::ParamBindings& bindings)
{
    if (! resolve)
        return;
    for (std::size_t i = 0; i < std::size(kMidiParamIds); ++i)
    {
        const core::ParamId id = resolve(kMidiParamIds[i]);
        if (! id.valid() || id.value >= field_.size())
            continue;
        field_[id.value] = static_cast<std::uint8_t>(i);
        bindings.bind(id, bridge::ParamTarget::Midi);
    }
    sceneQueue_ = resolve("scene.queue");
}

void MidiEngine::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    clock_.prepare(sampleRate_);
    in_.prepare();
    follower_.prepare(sampleRate_);
    out_.reset();
    clockOut_.prepare(sampleRate_);
    pendingStart_ = Pending::None;
    lostHandled_ = false;
    blockStart_ = 0;
    midiMask_ = 0;
    heldNote_.fill(-1);
    remoteScene_ = -1;
    for (int p = 0; p < core::midi::kMaxPorts; ++p)
        generation_[static_cast<std::size_t>(p)] = hub_.generation(p);
    if (correctionInstalled_ && w_.transport != nullptr)
        w_.transport->transport().setCorrection(nullptr, nullptr);
    correctionInstalled_ = following_ = false;
}

void MidiEngine::beginPeriod(std::int64_t firstFrame, std::int64_t hostNs, int frames) noexcept
{
    clock_.period(firstFrame, hostNs, frames);
    hub_.publishAnchor(clock_.anchor());
}

bool MidiEngine::onParam(const ParamChange& c) noexcept
{
    if (! c.param.valid() || c.param.value >= field_.size())
        return false;
    const std::uint8_t f = field_[c.param.value];
    if (f == kNotOurs)
        return false;
    const auto field = static_cast<F>(f);
    const bool on = c.value >= 0.5f;
    const int iv = toInt(c.value);
    const auto index = static_cast<int>(field);
    if (field <= F::OutSongPosition)
    {
        if (index < 4)
            inEnable_[static_cast<std::size_t>(index)] = on;
        else
            outEnable_[static_cast<std::size_t>(index - 4)] = on;
        return true;
    }
    switch (field)
    {
        case F::InChannel: rxChannel_[c.scope & 15] = static_cast<std::int8_t>(clampi(iv, 0, 16)); return true;
        case F::OutChannel: txChannel_[c.scope & 15] = static_cast<std::int8_t>(clampi(iv, 0, 16)); return true; // 0 = OFF
        case F::SyncSource: syncSource_ = clampi(iv, 0, 1); return true;
        case F::SyncPort: syncPort_ = clampi(iv, 1, core::midi::kMaxPorts) - 1; return true;
        case F::ClockLoss: clockLoss_ = clampi(iv, 0, 1); return true;
        case F::ClockWhileStopped: clockWhileStopped_ = on; return true;
        case F::OutLatency: hub_.setOutputLatencyMs(clampi(iv, 0, 100)); return true;
        case F::PortMode:
            portMode_[c.scope & 7] = static_cast<std::int8_t>(core::midi::legalPortMode(c.scope & 7, iv));
            hub_.setPortMode(c.scope & 7, portMode_[c.scope & 7]);
            hub_.bumpGeneration(c.scope & 7); // MID-047: a mode change ends the port's sounding notes
            return true;
        case F::PortOffset: hub_.setPortOffsetMs(c.scope & 7, clampi(iv, -50, 50)); return true;
        case F::RemoteSelectBlocked:
            remoteBlocked_ = on;
            if (on)
                remoteScene_ = -1; // SCN-051: a CC 0 waiting for its CC 32 is dropped
            return true;
        default: break;
    }
    if (c.scope == core::midi::kProjectOpenScope) // a project opens: its MIDI track values start from the defaults
    {
        for (auto& scene : tracks_)
            for (auto& t : scene)
                t = MidiTrackState{};
        remoteScene_ = -1;
        return true;
    }
    if (c.scope > 0xFF)
        return true; // a MIDI track value needs a track scope
    const int scene = (c.scope >> 4) & 15, track = c.scope & 15;
    MidiTrackState& s = tracks_[scene][track];
    switch (field)
    {
        case F::Note1:
            s.notes[0] = static_cast<std::int8_t>(clampi(iv, 0, 119)); // C0…B9
            if (w_.router != nullptr) // the reference of the track's SCALE pads (SET SCALE (MIDI), PAD-020)
                w_.router->setMidiNote1(scene, track, s.notes[0]);
            break;
        case F::Note2:
        case F::Note3:
        case F::Note4:
            s.notes[static_cast<std::size_t>(index - static_cast<int>(F::Note1))] =
                static_cast<std::int8_t>(clampi(iv, -127, 127)); // semitones from NOTE, 0 = off
            break;
        case F::Velocity: s.velocity = static_cast<std::uint8_t>(clampi(iv, 0, 127)); break;
        case F::Length: s.length = c.value < 0.25f ? 0.25f : (c.value > 64.0f ? 64.0f : c.value); break;
        case F::NoteBypass: s.noteBypass = on; break;
        case F::PitchBend:
            s.pitchBend = static_cast<std::int16_t>(clampi(iv, -8192, 8191));
            break;
        case F::PitchBendActive:
            s.pitchBendActive = on;
            break;
        case F::Aftertouch:
            s.aftertouch = static_cast<std::uint8_t>(clampi(iv, 0, 127));
            break;
        case F::AftertouchActive:
            s.aftertouchActive = on;
            break;
        case F::Cc1Bypass: s.cc1Bypass = on; break;
        case F::Cc2Bypass: s.cc2Bypass = on; break;
        case F::BankMsb: s.bankMsb = static_cast<std::int8_t>(clampi(iv, -1, 127)); break;
        case F::BankLsb: s.bankLsb = static_cast<std::int8_t>(clampi(iv, -1, 127)); break;
        case F::Program: s.program = static_cast<std::int8_t>(clampi(iv, -1, 127)); break;
        case F::Mode: s.mode = static_cast<std::uint8_t>(clampi(iv, 0, 1)); break;
        default:
        {
            // The CC1 (params 3–6 → slots 0–3) and CC2 (params 1–6 → slots 4–9) numbers, values, Active flags.
            int slot = -1, what = 0; // 0 number, 1 value, 2 active
            if (field >= F::Cc1Num3 && field <= F::Cc1Act6)
            {
                const int k = index - static_cast<int>(F::Cc1Num3);
                slot = k % 4;
                what = k / 4;
            }
            else if (field >= F::Cc2Num1 && field <= F::Cc2Act6)
            {
                const int k = index - static_cast<int>(F::Cc2Num1);
                slot = 4 + k % 6;
                what = k / 6;
            }
            if (slot < 0)
                break;
            const auto i = static_cast<std::size_t>(slot);
            if (what == 0)
                s.ccNumber[i] = static_cast<std::uint8_t>(clampi(iv, 0, 127));
            else if (what == 1)
                s.ccValue[i] = static_cast<std::uint8_t>(clampi(iv, 0, 127));
            else
                s.ccActive[i] = on;
            break;
        }
    }
    return true;
}

int MidiEngine::currentScene() const noexcept { return w_.router != nullptr ? w_.router->scene() : 0; }

bool MidiEngine::trackIs(int track, int mode) const noexcept
{
    if (w_.router == nullptr)
        return mode == static_cast<int>(trigger::TrackMode::Sample);
    return static_cast<int>(w_.router->settings(currentScene(), track).mode) == mode;
}

bool MidiEngine::trackIsSample(int track) const noexcept
{
    return trackIs(track, static_cast<int>(trigger::TrackMode::Sample));
}

std::uint32_t MidiEngine::outputPorts() const noexcept
{
    std::uint32_t ports = 0;
    const std::uint32_t connected = hub_.outputMask();
    for (int p = 0; p < core::midi::kMaxPorts; ++p)
        if ((connected >> p & 1u) && portMode_[static_cast<std::size_t>(p)] <= 2) // OUT, OUT + MERGE A/B
            ports |= 1u << p;
    return ports;
}

void MidiEngine::updateFollowing() noexcept
{
    const bool want = syncSource_ == 1 && inEnable_[0];
    if (want == following_)
        return;
    following_ = want;
    follower_.reset();
    pendingStart_ = Pending::None;
    lostHandled_ = false;
    if (w_.transport == nullptr)
        return;
    w_.transport->transport().setCorrection(want ? &MidiEngine::correction : nullptr, want ? this : nullptr);
    correctionInstalled_ = want;
}

void MidiEngine::correction(void* self, double ppq, double& bpm, double& phase) noexcept
{
    static_cast<MidiEngine*>(self)->correct(ppq, bpm, phase);
}

void MidiEngine::correct(double ppq, double& bpm, double& phase) noexcept
{
    if (! following_ || ! follower_.locked() || follower_.lost(blockStart_))
        return; // SYN-013: the last tempo is kept (the transport keeps its corrected rate)
    const double tempo = follower_.tempo();
    if (! (tempo > 0.0))
        return;
    double b = tempo;
    if (follower_.hasPosition())
    {
        const double err = follower_.ppqAt(static_cast<double>(blockStart_)) - ppq; // quarters
        if (std::fabs(err) > 0.5 / 24.0)
            phase = err; // far off (SPP, a lost stretch of clock): jump onto the clock
        else
        {
            const double qpf = tempo / (60.0 * sampleRate_);
            double steer = err / (qpf * kSteerFrames);
            steer = steer > kMaxSteer ? kMaxSteer : (steer < -kMaxSteer ? -kMaxSteer : steer);
            b = tempo * (1.0 + steer);
        }
    }
    bpm = b;
}

void MidiEngine::select(int scene, int pattern) noexcept
{
    if (remoteBlocked_ || w_.sink == nullptr || ! sceneQueue_.valid())
        return; // SCN-051
    int s = scene, p = pattern;
    if (w_.switcher != nullptr)
    {
        const auto& st = w_.switcher->state();
        s = s < 0 ? st.scene : s;
        p = p < 0 ? st.pattern : p;
    }
    s = s < 0 ? 0 : s;
    p = p < 0 ? 0 : p;
    w_.sink->applyParam(ParamChange {sceneQueue_, 0, static_cast<float>(s * 16 + p), 0});
    core::midi::UiEvent u;
    u.kind = core::midi::UiEvent::Kind::SceneSelect;
    u.scope = static_cast<std::uint16_t>(s * 16 + p);
    hub_.pushUi(u);
}

core::midi::CcKind MidiEngine::ccKind(int cc) const noexcept
{
    const core::midi::CcTable* table = cc_.table();
    return table != nullptr ? table->cc[static_cast<std::size_t>(cc & 127)].kind : core::midi::CcKind::None;
}

void MidiEngine::controlChange(int ch, int cc, int value, int frame) noexcept
{
    ++ccsIn_;
    if (cc_.table() == nullptr)
        return;
    const auto kind = ccKind(cc);
    if (kind == core::midi::CcKind::SceneSelect || kind == core::midi::CcKind::PatternSelect)
    {
        // SCN-050, manual p.88: the switch is fixed by the PATTERN (CC 32); CC 0 only holds the scene for it. On any
        // channel (our choice); value 0 is ignored.
        const int sel = core::midi::ccSelect(value);
        if (sel < 0 || remoteBlocked_) // SCN-051
            return;
        if (kind == core::midi::CcKind::SceneSelect)
        {
            remoteScene_ = sel;
            remoteSceneAt_ = blockStart_ + frame;
            return;
        }
        const int scene = remoteScene_; // −1: the current scene
        remoteScene_ = -1;
        select(scene, sel);
        return;
    }
    const int scene = currentScene();
    for (int t = 0; t < kTracks; ++t)
    {
        if (rxChannel_[static_cast<std::size_t>(t)] != ch + 1)
            continue;
        if (kind == core::midi::CcKind::AllNotesOff) // MID-023: the voices of every track on the channel, at its frame
        {
            if (w_.router != nullptr)
                w_.router->push(hit(t, 0, 0, trigger::TriggerKind::Stop, frame));
            heldNote_[static_cast<std::size_t>(t)] = -1;
            continue;
        }
        if (! (trackIsSample(t) || trackIs(t, static_cast<int>(trigger::TrackMode::Thru)) ||
               trackIs(t, static_cast<int>(trigger::TrackMode::Plugin))))
            continue; // MID-030: Sample / Thru / Plugin audio tracks only
        if (w_.fx == nullptr || w_.sink == nullptr)
            continue;
        ParamChange applied {};
        if (cc_.apply(scene, t, cc, value, *w_.fx, *w_.sink, applied) == CcMap::Result::Applied)
        {
            core::midi::UiEvent u;
            u.kind = core::midi::UiEvent::Kind::Param;
            u.scope = applied.scope;
            u.param = applied.param.value;
            u.value = applied.value;
            hub_.pushUi(u);
        }
    }
}

void MidiEngine::channelMessage(const MidiEvent& e, int frame) noexcept
{
    if (! inEnable_[2]) // IN NOTE/CC (MID-012)
        return;
    const int type = e.bytes[0] & 0xF0, ch = e.bytes[0] & 15;
    const int d1 = e.bytes[1] & 127, d2 = e.bytes[2] & 127;
    if (remoteScene_ >= 0 &&
        blockStart_ + frame - remoteSceneAt_ > static_cast<std::int64_t>(kRemoteSelectWait * sampleRate_))
        remoteScene_ = -1; // the CC 32 never came
    if (remoteScene_ >= 0) // manual p.88: other messages are ignored while a CC 0 waits for its CC 32
    {
        const auto kind = type == 0xB0 ? ccKind(d1) : core::midi::CcKind::None;
        const bool select = kind == core::midi::CcKind::SceneSelect || kind == core::midi::CcKind::PatternSelect;
        const bool ends = type == 0x80 || (type == 0x90 && d2 == 0) || kind == core::midi::CcKind::AllNotesOff;
        if (! select && ! ends)
            return;
    }
    switch (type)
    {
    case 0x90:
    case 0x80:
    {
        const bool on = type == 0x90 && d2 > 0;
        if (w_.router == nullptr)
            return;
        // The note that started a track's sounding voice (port, channel, note); only its Note Off releases it.
        const auto key = static_cast<std::int16_t>((e.port & 7) << 11 | ch << 7 | d1);
        if (! on)
        {
            for (int t = 0; t < kTracks; ++t)
                if (heldNote_[static_cast<std::size_t>(t)] == key)
                {
                    w_.router->push(hit(t, 0, 0, trigger::TriggerKind::Release, frame));
                    heldNote_[static_cast<std::size_t>(t)] = -1;
                }
            return;
        }
        if (d1 <= 15) // MID-020: notes 0–15, any channel → track n + 1
        {
            if (! trackIsSample(d1) && ! trackIs(d1, static_cast<int>(trigger::TrackMode::Plugin))) // MID-022
                return;
            w_.router->push(hit(d1, d2, 0, trigger::TriggerKind::Full, frame));
            heldNote_[static_cast<std::size_t>(d1)] = key;
            ++notesIn_;
            return;
        }
        if (d1 < 24 || d1 > 72)
            return;
        for (int t = 0; t < kTracks; ++t) // MID-021: notes 24–72 on the track's receive channel, note − 48
        {
            if (rxChannel_[static_cast<std::size_t>(t)] != ch + 1 ||
                (! trackIsSample(t) && ! trackIs(t, static_cast<int>(trigger::TrackMode::Plugin))))
                continue;
            int semis = d1 - 48;
            if (w_.pitchPreserving != nullptr && w_.pitchPreserving(w_.user, t))
                semis = clampi(semis, -12, 12);
            w_.router->push(hit(t, d2, semis, trigger::TriggerKind::Full, frame));
            heldNote_[static_cast<std::size_t>(t)] = key;
            ++notesIn_;
        }
        return;
    }
    case 0xB0:
        controlChange(ch, d1, d2, frame);
        return;
    case 0xE0: // MID-032
    {
        if (w_.ribbon == nullptr)
            return;
        const int v14 = d1 | (d2 << 7);
        for (int t = 0; t < kTracks; ++t)
            if (rxChannel_[static_cast<std::size_t>(t)] == ch + 1)
                w_.ribbon->onPitchBend(t, v14);
        return;
    }
    default:
        return; // program change, pressure: not received (research docs/midi-implementation.md)
    }
}

void MidiEngine::handle(const DueEvent& d, BlockContext&) noexcept
{
    const MidiEvent& e = d.event;
    const std::uint8_t status = e.bytes[0];
    activity_ |= 1u << (e.port & 7);
    if (status < 0xF0)
    {
        channelMessage(e, d.frame);
        return;
    }
    const bool fromSync = ! following_ || e.port == syncPort_;
    auto* tr = w_.transport;
    switch (status)
    {
        case 0xF8: // timing clock
            if (! following_ || e.port != syncPort_)
                return;
            follower_.onTick(d.exactFrame);
            ++ticksIn_;
            lostHandled_ = false;
            if (pendingStart_ != Pending::None && tr != nullptr)
            {
                tr->command(TransportCmd {pendingStart_ == Pending::Start ? TransportCmd::Play : TransportCmd::Continue, 0.0});
                pendingStart_ = Pending::None;
            }
            return;
        case 0xFA: // Start
            if (! inEnable_[1] || ! fromSync || tr == nullptr)
                return;
            if (tr->playing())
                tr->command(TransportCmd {TransportCmd::Stop, 1.0});
            follower_.setNextTickPosition(0.0);
            if (following_)
                pendingStart_ = Pending::Start; // the next tick is beat 0
            else
                tr->command(TransportCmd {TransportCmd::Play, 0.0});
            return;
        case 0xFB: // Continue
            if (! inEnable_[1] || ! fromSync || tr == nullptr)
                return;
            follower_.setNextTickPosition(tr->ppq());
            if (following_)
                pendingStart_ = Pending::Continue;
            else
                tr->command(TransportCmd {TransportCmd::Continue, 0.0});
            return;
        case 0xFC: // Stop: pause (a later Continue resumes here, Start restarts from 0)
            if (! inEnable_[1] || ! fromSync || tr == nullptr)
                return;
            pendingStart_ = Pending::None;
            follower_.clearPosition();
            tr->command(TransportCmd {TransportCmd::Stop, 0.0});
            return;
        case 0xF2: // Song Position Pointer (sixteenths), obeyed while stopped
        {
            if (! inEnable_[3] || ! fromSync || tr == nullptr || tr->playing())
                return;
            const int pos16 = (e.bytes[1] & 127) | ((e.bytes[2] & 127) << 7);
            if (w_.arrangement != nullptr && w_.arrangement->engaged())
                w_.arrangement->locate16th(pos16); // ARR-008
            else
                tr->command(TransportCmd {TransportCmd::Locate, pos16 / 4.0});
            follower_.setNextTickPosition(pos16 / 4.0);
            return;
        }
        default: return;
    }
}

void MidiEngine::processIn(BlockContext& ctx) noexcept
{
    blockStart_ = static_cast<std::int64_t>(ctx.blockIndex) * kBlockFrames;
    if (const auto* t = hub_.ccTable(); t != cc_.table())
        cc_.setTable(t);
    updateFollowing();
    in_.collect(hub_, 0xFFu, clock_, blockStart_, inputLatency());
    activity_ = 0;
    for (const DueEvent* d = in_.begin(); d != in_.end(); ++d)
        handle(*d, ctx);
    if (activity_ != 0)
        for (std::uint32_t m = activity_, p = 0; m != 0; ++p, m >>= 1)
            if (m & 1u)
            {
                core::midi::UiEvent u;
                u.kind = core::midi::UiEvent::Kind::Activity;
                u.port = static_cast<std::uint8_t>(p);
                hub_.pushUi(u);
            }
    if (! following_)
        return;
    follower_.updateDisplay(blockStart_);
    if (follower_.displayTempo() != shownTempo_)
    {
        shownTempo_ = follower_.displayTempo();
        core::midi::UiEvent u;
        u.kind = core::midi::UiEvent::Kind::Tempo;
        u.value = static_cast<float>(shownTempo_);
        hub_.pushUi(u);
    }
    if (follower_.ticks() > 0 && follower_.lost(blockStart_) && ! lostHandled_)
    {
        lostHandled_ = true; // SYN-013: keep the last tempo (default) or stop
        if (clockLoss_ == 1 && w_.transport != nullptr && w_.transport->playing())
            w_.transport->command(TransportCmd {TransportCmd::Stop, 1.0});
    }
}

void MidiEngine::trackOut(const BlockContext& ctx, const stages::BlockBuffers& b, std::uint32_t ports) noexcept
{
    if (w_.router == nullptr)
        return;
    const int scene = currentScene();
    // MIDI and TORAIZ AS-1 tracks send; an AS-1 track sends NOTE alone (the AS-1 plays no chords: no NOTE2–4).
    std::uint16_t mask = 0, mono = 0;
    for (int t = 0; t < kTracks; ++t)
    {
        const auto mode = w_.router->settings(scene, t).mode;
        if (mode == trigger::TrackMode::Midi || mode == trigger::TrackMode::Instrument)
            mask = static_cast<std::uint16_t>(mask | (1u << t));
        if (mode == trigger::TrackMode::Instrument)
            mono = static_cast<std::uint16_t>(mono | (1u << t));
    }
    for (std::uint32_t left = static_cast<std::uint32_t>(midiMask_ & ~mask), t = 0; left != 0; ++t, left >>= 1)
        if (left & 1u)
            out_.trackNotesOff(static_cast<int>(t)); // MID-047: the track left MIDI mode
    midiMask_ = mask;
    if (! outEnable_[2] || ports == 0 || mask == 0) // OUT NOTE/CC (MID-012)
        return;
    const auto send = [&](std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, int size, std::int64_t t) {
        for (std::uint32_t m = ports, p = 0; m != 0; ++p, m >>= 1)
            if (m & 1u)
                out_.message(static_cast<int>(p), b0, b1, b2, size, t);
    };
    if (b.timing.sceneChanged) // MID-050: bank MSB, LSB, program when the scene becomes current
        for (int t = 0; t < kTracks; ++t)
        {
            if (! (mask >> t & 1u) || txChannel_[static_cast<std::size_t>(t)] == 0) // send channel OFF: nothing
                continue;
            const auto& s = tracks_[scene][t];
            const auto ch = static_cast<std::uint8_t>((txChannel_[static_cast<std::size_t>(t)] - 1) & 15);
            if (s.bankMsb >= 0)
                send(static_cast<std::uint8_t>(0xB0 | ch), 0, static_cast<std::uint8_t>(s.bankMsb), 3, blockStart_);
            if (s.bankLsb >= 0)
                send(static_cast<std::uint8_t>(0xB0 | ch), 32, static_cast<std::uint8_t>(s.bankLsb), 3, blockStart_);
            if (s.program >= 0)
                send(static_cast<std::uint8_t>(0xC0 | ch), static_cast<std::uint8_t>(s.program), 0, 2, blockStart_);
        }
    // MID-040 / SEQ-050: the note module per Full/Half trigger of a MIDI track (step, pad, MIDI, retrigger).
    const double framesPerStep = 0.25 * 60.0 / (ctx.bpm > 0.0 ? ctx.bpm : 120.0) * sampleRate_;
    for (const auto& e : w_.router->blockEvents())
    {
        const int t = e.track;
        if (! (mask >> t & 1u))
            continue;
        if (e.kind == trigger::TriggerKind::Stop)
        {
            out_.trackNotesOff(t);
            continue;
        }
        if (txChannel_[static_cast<std::size_t>(t)] == 0) // send channel OFF (utility.md): no new notes
            continue;
        if (e.kind != trigger::TriggerKind::Full && e.kind != trigger::TriggerKind::Half)
            continue;
        const auto& s = tracks_[scene][t]; // noteBypass is kept for the file formats: NOTE is not bypassable
        int transpose = w_.perf != nullptr ? w_.perf->transpose(t) : 0;
        // A step's PITCH lock transposes the notes (a recorded MIDI-scale pad or played note keeps its pitch, SEQ-041).
        if (float lock = 0.0f; e.locks != nullptr && e.locks->values != nullptr && e.locks->get(kPitchLockSlot, lock))
            transpose += toInt(lock);
        const int vel = s.velocity > 0 ? s.velocity : (e.velocity > 0 ? e.velocity : 100);
        const auto length = static_cast<std::int64_t>(std::llround(static_cast<double>(s.length) * framesPerStep));
        const std::int64_t at = blockStart_ + e.frame;
        const auto ch = static_cast<std::uint8_t>((txChannel_[static_cast<std::size_t>(t)] - 1) & 15);
        // MIDI CC1 / CC2 (the manual's 2.4, p.51: "sent when pressing pads of a MIDI track or triggered by a
        // sequence"): the module's Active parameters, unless bypassed, just before the trigger's notes, at their frame.
        // PARAMETER 1 / 2 of CC1 are pitch bend and aftertouch.
        if (! s.cc1Bypass && s.pitchBendActive)
        {
            const int v = s.pitchBend + 8192;
            send(static_cast<std::uint8_t>(0xE0 | ch), static_cast<std::uint8_t>(v & 127),
                 static_cast<std::uint8_t>((v >> 7) & 127), 3, at);
        }
        if (! s.cc1Bypass && s.aftertouchActive)
            send(static_cast<std::uint8_t>(0xD0 | ch), s.aftertouch, 0, 2, at);
        for (int i = 0; i < 10; ++i)
            if (s.ccActive[static_cast<std::size_t>(i)] && ! (i < 4 ? s.cc1Bypass : s.cc2Bypass))
                send(static_cast<std::uint8_t>(0xB0 | ch), s.ccNumber[static_cast<std::size_t>(i)],
                     s.ccValue[static_cast<std::size_t>(i)], 3, at);
        const int chord = (mono >> t & 1u) ? 1 : 4;
        std::array<int, 4> played {-1, -1, -1, -1};
        for (int i = 0; i < chord; ++i)
        {
            const int offset = s.notes[static_cast<std::size_t>(i)];
            if (i > 0 && offset == 0)
                continue; // NOTE2–4 OFF
            const int note = s.notes[0] + (i > 0 ? offset : 0) + e.semitones + transpose;
            if (note < 0 || note > 127)
                continue;
            // Two of NOTE…NOTE4 on one note number send it once (a second Note On would end the first at once).
            if (std::find(played.begin(), played.end(), note) != played.end())
                continue;
            played[static_cast<std::size_t>(i)] = note;
            for (std::uint32_t m = ports, p = 0; m != 0; ++p, m >>= 1)
                if (m & 1u)
                    out_.note(static_cast<int>(p), ch, note, vel, at, length, t);
        }
    }
}

void MidiEngine::processOut(const BlockContext& ctx, const stages::BlockBuffers& b) noexcept
{
    out_.beginBlock(blockStart_);
    for (int p = 0; p < core::midi::kMaxPorts; ++p) // MID-047: a port change ends that port's sounding notes
    {
        const std::uint32_t g = hub_.generation(p);
        if (g != generation_[static_cast<std::size_t>(p)])
        {
            generation_[static_cast<std::size_t>(p)] = g;
            out_.allNotesOff(p);
        }
    }
    const bool hardStop = w_.transport != nullptr && w_.transport->hardStopThisBlock();
    if (ctx.stoppedThisBlock || hardStop)
        out_.allNotesOff(); // MID-047: stop
    const std::uint32_t ports = outputPorts();
    trackOut(ctx, b, ports);
    if (w_.transport != nullptr)
    {
        const ClockOutSettings s {outEnable_[0], outEnable_[1], outEnable_[3], clockWhileStopped_};
        const std::int64_t spp16 = w_.arrangement != nullptr && w_.arrangement->engaged()
                                       ? w_.arrangement->songPositionAt(ctx.ppqStart)
                                       : static_cast<std::int64_t>(std::llround(ctx.ppqStart * 4.0));
        clockOut_.process(ctx, s, ports, blockStart_, w_.transport->transport().ppqPerFrame(),
                          w_.transport->locatedThisBlock(), spp16, out_);
    }
    out_.endBlock();
}

MidiStats MidiEngine::stats() const noexcept
{
    MidiStats s;
    s.received = in_.received();
    s.late = in_.late();
    s.notesIn = notesIn_;
    s.ccsIn = ccsIn_;
    s.ticksIn = ticksIn_;
    s.sent = out_.sent();
    s.dropped = out_.dropped() + in_.dropped();
    return s;
}
} // namespace engine::midi
