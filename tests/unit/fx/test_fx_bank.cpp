// FxProcessor and FxSlotBank behaviour (spec effect-shell.md FX-023…050, parameters.md FX-056/076…094, README.md
// FX-002…009) with the built-in test effects (GainTest, None; flag "fx.test") and a probe effect. Every block runs
// under core::rt::ScopedRealtimeThread: an allocation on the audio path aborts the test (APP_RT_CHECKS).
#include "tests/unit/fx/FxTestUtil.h"

#include "core/util/GainLaw.h"
#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

using namespace engine::fx;

namespace
{
constexpr int N = engine::kBlockFrames;
struct Block
{
    alignas(16) float l[N];
    alignas(16) float r[N];
    void fill(float a, float b)
    {
        for (int i = 0; i < N; ++i)
        {
            l[i] = a * std::sin(0.05f * static_cast<float>(i + 1));
            r[i] = b * std::cos(0.03f * static_cast<float>(i + 1));
        }
    }
};
bool equal(const Block& a, const Block& b)
{
    return std::memcmp(&a, &b, sizeof(Block)) == 0;
}
bool silent(const Block& a)
{
    for (int i = 0; i < N; ++i)
        if (a.l[i] != 0.0f || a.r[i] != 0.0f)
            return false;
    return true;
}
constexpr std::uint16_t trackScope(int scene, int track)
{
    return static_cast<std::uint16_t>((scene << 4) | track);
}
int gainTestType(const FxSlotBank& bank)
{
    return typeIndexOf(bank.insertTypes(), "gainTest");
}
int noneType(const FxSlotBank& bank)
{
    return typeIndexOf(bank.insertTypes(), "none");
}
} // namespace

TEST_CASE("fx.bank: Off slots are idle and leave the buses as they are (FX-023/043)", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const auto ctx = fxtest::context();
    Block in, b;
    in.fill(0.5f, -0.4f);
    core::rt::ScopedRealtimeThread rt;
    for (int k = 0; k < 4; ++k)
    {
        b = in;
        CHECK(bank.insertIdle(3));
        bank.processInsert(3, b.l, b.r, ctx);
        CHECK(equal(b, in));
        bank.processMaster(b.l, b.r, ctx);
        CHECK(equal(b, in));
        CHECK_FALSE(bank.processSend(b.l, b.r, ctx)); // Send Off = silent return (the caller skips it)
        CHECK(bank.sendIdle());
    }
    engine::mixer::Hooks h;
    bank.fillHooks(h);
    for (auto* p : h.insert)
        CHECK(p == nullptr);
    CHECK(h.sendFx == nullptr);
    CHECK(h.masterFx == nullptr);
}

TEST_CASE("fx.bank: the route table resolves slot, type and internal index", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const auto& r = bank.routes();
    const auto& g = r.find(fxtest::idOf("fx2.gainTest.gain"));
    CHECK(g.kind == FxRoute::Kind::Knob);
    CHECK(g.slotKind == SlotKind::Insert);
    CHECK(g.unit == 1);
    CHECK(g.type == gainTestType(bank));
    CHECK(g.index == 0);
    const auto& m = r.find(fxtest::idOf("master.none.mix"));
    CHECK(m.kind == FxRoute::Kind::Knob);
    CHECK(m.slotKind == SlotKind::Master);
    CHECK(m.index == 5);
    CHECK(r.find(fxtest::idOf("send.type")).kind == FxRoute::Kind::SlotType);
    CHECK(r.find(fxtest::idOf("fx1.bypass")).kind == FxRoute::Kind::SlotBypass);
    CHECK(r.find(fxtest::idOf("fx.mode")).kind == FxRoute::Kind::Mode);
    CHECK(r.find(fxtest::idOf("track.volume")).kind == FxRoute::Kind::None);
}

TEST_CASE("fx.bank: activation fade-in, sequential type change 3 blocks later, bypass (FX-024/041/044)", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const auto ctx = fxtest::context();
    const int gt = gainTestType(bank), none = noneType(bank);
    Block in, b;
    in.fill(0.5f, 0.5f);
    const std::uint16_t sc = trackScope(0, 5);
    bank.onParamChange(fxtest::change("fx1.gainTest.gain", 0.0f, sc)); // index 0: silence
    bank.onParamChange(fxtest::change("fx1.type", static_cast<float>(gt + 1), sc));
    auto& proc = bank.slot(insertSlot(5, 0));
    CHECK(proc.current() == gt); // from Off: current at once
    {
        core::rt::ScopedRealtimeThread rt;
        for (int k = 0; k < 4; ++k)
        {
            b = in;
            bank.processInsert(5, b.l, b.r, ctx);
            if (k == 0)
                CHECK(b.l[0] == in.l[0]); // the first ramp sample is the start value: dry
            if (k >= 3)
                CHECK(silent(b)); // after N_shell = 147: wet only, gain 0 (GainLaw index 0)
        }
    }
    CHECK(core::GainLaw::gain(0) == 0.0f);
    bank.onParamChange(fxtest::change("fx1.type", static_cast<float>(none + 1), sc));
    CHECK(proc.pending() == none);
    int swapBlock = -1;
    {
        core::rt::ScopedRealtimeThread rt;
        for (int k = 0; k < 6; ++k)
        {
            b = in;
            bank.processInsert(5, b.l, b.r, ctx);
            if (proc.current() == none && swapBlock < 0)
                swapBlock = k;
        }
    }
    CHECK(swapBlock == 3); // FX-041: the swap happens 3 blocks after the request
    // None at its default Mix 0 (CrossfadeK5, dry 1 wet 0) passes the input; the processed path must be exact
    b = in;
    bank.processInsert(5, b.l, b.r, ctx);
    CHECK(equal(b, in));
    // GainTest back (gain still 0, kept per type, FX-006), then bypass: exactly dry after N_mute
    bank.onParamChange(fxtest::change("fx1.type", static_cast<float>(gt + 1), sc));
    bank.onParamChange(fxtest::change("fx1.bypass", 1.0f, sc));
    core::rt::ScopedRealtimeThread rt;
    for (int k = 0; k < 10; ++k)
    {
        b = in;
        bank.processInsert(5, b.l, b.r, ctx);
    }
    CHECK(proc.current() == gt);
    CHECK(bank.stored(insertSlot(5, 0), gt, 0) == 0.0f);
    CHECK(equal(b, in));              // bypassed: the dry input, bit for bit
    CHECK(proc.shell(gt)->running()); // the effect keeps running behind the bypass
}

TEST_CASE("fx.bank: Send Off mutes, Send runs on the bus; FX mode disables the other slot (FX-008/043/047)",
          "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const auto ctx = fxtest::context();
    const int none = typeIndexOf(bank.sendTypes(), "none");
    bank.onParamChange(fxtest::change("send.none.mix", 1.0f, 0)); // wet only = the bus (None is the identity)
    bank.onParamChange(fxtest::change("send.type", static_cast<float>(none + 1), 0));
    bank.onParamChange(fxtest::change("master.type", static_cast<float>(none + 1), 0));
    Block in, b;
    in.fill(0.3f, 0.6f);
    core::rt::ScopedRealtimeThread rt;
    for (int k = 0; k < 8; ++k)
    {
        b = in;
        CHECK(bank.processSend(b.l, b.r, ctx));
    }
    CHECK(equal(b, in));
    bank.onParamChange(fxtest::change("fx.mode", 2.0f, 0)); // Master mode: the Send FX is disabled
    CHECK(bank.fxMode() == FxMode::Master);
    CHECK(bank.slot(kSendSlot).disabled());
    CHECK_FALSE(bank.slot(kMasterSlot).disabled());
    bool silentFrom147 = true;
    for (int k = 0; k < 6; ++k)
    {
        b = in;
        const bool audible = bank.processSend(b.l, b.r, ctx);
        if (k >= 3 && audible && ! silent(b))
            silentFrom147 = false;
    }
    CHECK(silentFrom147);
    CHECK(bank.sendIdle());
    bank.onParamChange(fxtest::change("fx.mode", 1.0f, 0)); // Send mode: Master disabled, Send re-activated
    CHECK(bank.slot(kMasterSlot).disabled());
    CHECK_FALSE(bank.slot(kSendSlot).disabled());
    for (int k = 0; k < 8; ++k)
    {
        b = in;
        bank.processSend(b.l, b.r, ctx);
    }
    CHECK(equal(b, in));
    CHECK(bank.slot(kSendSlot).shell(none)->running());
    for (int k = 0; k < 4; ++k)
    {
        b = in;
        bank.processMaster(b.l, b.r, ctx);
    }
    CHECK(equal(b, in)); // a disabled Master passes the bus bit for bit after its stop fade
    CHECK(bank.masterIdle());
}

TEST_CASE("fx.bank: a bypassed Master passes the main bus dry, a bypassed Send the send bus (FX-046)", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const auto ctx = fxtest::context();
    const int mt = typeIndexOf(bank.masterTypes(), "gainTest"), st = typeIndexOf(bank.sendTypes(), "gainTest");
    for (const char* slot : {"master", "send"}) // GainTest at index 0: the effect alone is silence
    {
        bank.onParamChange(fxtest::change(std::string(slot) + ".gainTest.gain", 0.0f, 0));
        bank.onParamChange(fxtest::change(std::string(slot) + ".type",
                                          static_cast<float>((slot[0] == 'm' ? mt : st) + 1), 0));
    }
    Block in, m, s;
    in.fill(0.4f, 0.7f);
    auto blocks = [&](int n) {
        core::rt::ScopedRealtimeThread rt;
        for (int k = 0; k < n; ++k)
        {
            m = in;
            s = in;
            bank.processMaster(m.l, m.r, ctx);
            CHECK(bank.processSend(s.l, s.r, ctx));
        }
    };
    blocks(6);
    CHECK(silent(m));
    CHECK(silent(s));
    bank.onParamChange(fxtest::change("master.bypass", 1.0f, 0));
    bank.onParamChange(fxtest::change("send.bypass", 1.0f, 0));
    CHECK(bank.slot(kMasterSlot).bypassed());
    CHECK(bank.slot(kSendSlot).bypassed());
    blocks(1); // N_mute = 132 > 64: the first block is part of the crossfade, never a step to the dry signal
    CHECK(m.l[0] == 0.0f);
    CHECK(m.l[N - 1] != 0.0f);
    CHECK_FALSE(equal(m, in));
    CHECK(s.l[0] == 0.0f);
    CHECK_FALSE(equal(s, in));
    blocks(3); // the ramp ends at frame 132: the last block (192…255) is dry
    CHECK(equal(m, in)); // bypassed: the dry bus, bit for bit
    CHECK(equal(s, in));
    CHECK(bank.slot(kMasterSlot).shell(mt)->running()); // the effects keep running behind the bypass
    CHECK(bank.slot(kSendSlot).shell(st)->running());
    bank.onParamChange(fxtest::change("master.bypass", 0.0f, 0));
    bank.onParamChange(fxtest::change("send.bypass", 0.0f, 0));
    blocks(4);
    CHECK(silent(m));
    CHECK(silent(s));
    // An Off Master stays idle once its bypass ramp has settled
    bank.onParamChange(fxtest::change("master.type", 0.0f, 0));
    bank.onParamChange(fxtest::change("master.bypass", 1.0f, 0));
    blocks(8);
    CHECK(equal(m, in));
    CHECK(bank.masterIdle());
}

TEST_CASE("fx.bank: step locks, edits during a lock, type-tagged locks, LFO amount (FX-079…085)", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const int gt = gainTestType(bank), none = noneType(bank);
    const std::uint16_t sc = trackScope(0, 2);
    const int s = insertSlot(2, 1);
    bank.onParamChange(fxtest::change("fx2.type", static_cast<float>(gt + 1), sc));
    bank.onParamChange(fxtest::change("fx2.gainTest.gain", 0.25f, sc));
    CHECK(bank.effective(s, 0) == 0.25f);
    FxStepLocks lk;
    lk.type[1] = static_cast<std::int8_t>(gt);
    lk.mask[1] = 1u;
    lk.value[1][0] = 0.75f;
    bank.applyStepLocks(2, lk);
    CHECK(bank.effective(s, 0) == 0.75f);
    CHECK(bank.slot(s).shell(gt)->param(0) == 0.75f);
    bank.onParamChange(fxtest::change("fx2.gainTest.gain", 0.5f, sc)); // FX-081: stored only
    CHECK(bank.stored(s, gt, 0) == 0.5f);
    CHECK(bank.effective(s, 0) == 0.75f);
    bank.applyStepLocks(2, FxStepLocks{}); // next trigger without a lock: back to the stored value (FX-079)
    CHECK(bank.effective(s, 0) == 0.5f);
    CHECK(bank.slot(s).shell(gt)->param(0) == 0.5f);
    FxStepLocks other = lk;
    other.type[1] = static_cast<std::int8_t>(none); // FX-080: a lock of another type does nothing
    bank.applyStepLocks(2, other);
    CHECK(bank.effective(s, 0) == 0.5f);
    bank.setModulation(2, 1, gt, 0, 0.75f); // FX-084: clamp(base + amount)
    CHECK(bank.slot(s).shell(gt)->param(0) == 1.0f);
    bank.setModulation(2, 1, gt, 0, -0.25f);
    CHECK(bank.slot(s).shell(gt)->param(0) == 0.25f);
    bank.setModulation(2, 1, none, 0, 0.5f); // FX-085: another type's destination
    CHECK(bank.slot(s).shell(gt)->param(0) == 0.5f);
    // ParamChanges of a scene that is not playing are not applied (the scene switch installs them)
    bank.onParamChange(fxtest::change("fx2.gainTest.gain", 0.1f, trackScope(3, 2)));
    CHECK(bank.stored(s, gt, 0) == 0.5f);
}

TEST_CASE("fx.bank: applyScene installs types, parameters, bypass and FX mode (FX-004/045)", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const int gt = gainTestType(bank);
    auto st = std::make_unique<FxSlotState>();
    st->scene = 4;
    st->fxMode = 2;
    st->slot[static_cast<std::size_t>(insertSlot(7, 0))].type = static_cast<std::int8_t>(gt);
    st->slot[static_cast<std::size_t>(insertSlot(7, 0))].bypass = 1;
    st->slot[static_cast<std::size_t>(insertSlot(7, 0))].params[0] = 0.3f;
    st->slot[kMasterSlot].type = static_cast<std::int8_t>(typeIndexOf(bank.masterTypes(), "gainTest"));
    {
        core::rt::ScopedRealtimeThread rt;
        bank.applyScene(*st);
    }
    const auto& p = bank.slot(insertSlot(7, 0));
    CHECK(p.current() == gt);
    CHECK(p.bypassed());
    CHECK(p.shell(gt)->param(0) == 0.3f);
    CHECK(bank.fxMode() == FxMode::Master);
    CHECK(bank.slot(kSendSlot).disabled());
    bank.onParamChange(fxtest::change("fx1.gainTest.gain", 0.6f, trackScope(4, 7))); // scene 4 is now playing
    CHECK(bank.stored(insertSlot(7, 0), gt, 0) == 0.6f);
}

TEST_CASE("fx.bank: MIDI tracks disable both inserts; tails keep a silent track running (FX-002)", "[fx][bank]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const int gt = gainTestType(bank);
    bank.onParamChange(fxtest::change("fx1.type", static_cast<float>(gt + 1), trackScope(0, 9)));
    engine::BlockContext bc{44100.0, 64, 0, 0, 120.0, false, false, false, 0u, {}, 0};
    bc.triggerOffset.fill(-1);
    auto ctx = bank.beginBlock(bc, 0);
    auto b = std::make_unique<engine::stages::BlockBuffers>();
    b->activeTracks = 0;
    CHECK(bank.claimTailTracks(*b) == (1u << 9)); // FX1 of track 9 is starting: it runs although the source is silent
    engine::mixer::Hooks h;
    bank.fillHooks(h);
    CHECK(h.insert[9] != nullptr);
    CHECK(h.insert[8] == nullptr);
    {
        core::rt::ScopedRealtimeThread rt;
        for (int k = 0; k < 4; ++k)
            h.insert[9]->process(b->track[9].l, b->track[9].r);
    }
    ctx = bank.beginBlock(bc, static_cast<std::uint16_t>(1u << 9));
    CHECK(ctx.midiTrackMask == (1u << 9));
    CHECK(bank.slot(insertSlot(9, 0)).disabled());
    CHECK(bank.slot(insertSlot(9, 1)).disabled());
    Block blk;
    blk.fill(0.5f, 0.5f);
    for (int k = 0; k < 4; ++k)
        bank.processInsert(9, blk.l, blk.r, ctx);
    CHECK(bank.insertIdle(9));
    bank.beginBlock(bc, 0);
    CHECK_FALSE(bank.slot(insertSlot(9, 0)).disabled());
}

namespace
{
struct ProbeLog
{
    int resets = 0, clears = 0, retriggers = 0, params = 0;
    std::uint32_t quirks = 0;
    float last[6]{};
};
ProbeLog gProbe;

class Probe final : public EffectBase
{
public:
    void prepare(double, int) override {}
    void reset() noexcept override { ++gProbe.resets; }
    void setParam(int k, float v) noexcept override
    {
        ++gProbe.params;
        gProbe.last[k] = v;
    }
    void setOption(int which, int v) noexcept override
    {
        if (which == kClearState && v != 0)
            ++gProbe.clears;
    }
    void process(float* l, float* r, const ProcessContext&) noexcept override
    {
        for (int i = 0; i < N; ++i)
        {
            l[i] *= 2.0f;
            r[i] *= 2.0f;
        }
    }
    void retrigger() noexcept override { ++gProbe.retriggers; }
    void setQuirkProfile(std::uint32_t m) noexcept override { gProbe.quirks = m; }
};
} // namespace

TEST_CASE("fx.shell: activation sends the full parameter set and resets; quirk and clear at activation", "[fx][shell]")
{
    gProbe = {};
    Probe probe;
    FxShell shell;
    alignas(16) float cl[N], cr[N];
    shell.prepare(probe, ShellPolicy{0, static_cast<std::uint8_t>(MixLaw::CrossfadeK5), false, false, false}, 48000.0,
                  SlotKind::Insert, cl, cr);
    CHECK_FALSE(shell.running());
    shell.setParam(5, 0.25f);
    shell.setParam(2, 7.0f); // FX-057: clamped
    CHECK(shell.param(2) == 1.0f);
    CHECK(shell.dry() == 0.75f);
    CHECK(shell.wet() == 0.25f);
    shell.setQuirkProfile(kQuirkProfileReference);
    shell.clearOnNextActivation();
    const int before = gProbe.params;
    shell.activate();
    CHECK(gProbe.params - before == 6); // FX-056
    CHECK(gProbe.resets == 1);
    CHECK(gProbe.clears == 1);
    CHECK(gProbe.quirks == kQuirkProfileReference);
    CHECK(gProbe.last[2] == 1.0f);
    Block b, in;
    in.fill(0.5f, 0.5f);
    for (int k = 0; k < 4; ++k)
    {
        b = in;
        shell.process(b.l, b.r, fxtest::context(48000.0));
    }
    for (int i = 0; i < N; ++i) // steady: f32(f32(x·0.75) + f32(2x·0.25))
    {
        const float p = in.l[i] * 0.75f, q = (in.l[i] * 2.0f) * 0.25f;
        CHECK(b.l[i] == p + q);
    }
    shell.stop();
    for (int k = 0; k < 3; ++k) // 160 samples at 48 kHz
    {
        b = in;
        shell.process(b.l, b.r, fxtest::context(48000.0));
    }
    CHECK_FALSE(shell.running());
    b = in;
    CHECK_FALSE(shell.process(b.l, b.r, fxtest::context(48000.0)));
    CHECK(equal(b, in));
}

TEST_CASE("fx.bank: second STOP fades out, clears the effects and fades back in (FX-036); transport restart (FX-089)",
          "[fx][bank]")
{
    gProbe = {};
    EffectRegistry reg;
    reg.add({.id = "probe",
             .name = "PROBE",
             .slots = kAllSlotKinds,
             .params = {},
             .options = {},
             .visual = {},
             .shell = {0, static_cast<std::uint8_t>(MixLaw::WetOnly), false, false, false},
             .create = &make<Probe>});
    FxSlotBank bank;
    const std::string_view ids[] = {"fx1.type", "master.type"};
    bank.build(reg, ids);
    REQUIRE(bank.prepare(44100.0, N));
    bank.onParamChange({core::ParamId{1}, 0, 1.0f, 0}); // master.type = probe
    bank.onParamChange({core::ParamId{0}, 0, 1.0f, 0}); // fx1.type of track 0
    const auto ctx = fxtest::context();
    Block b, in;
    in.fill(0.5f, 0.5f);
    for (int k = 0; k < 4; ++k)
    {
        b = in;
        bank.processMaster(b.l, b.r, ctx);
    }
    CHECK(b.l[10] == in.l[10] * 2.0f);
    engine::BlockContext bc{44100.0, 64, 0, 0, 120.0, true, true, false, 0u, {}, 0};
    bank.beginBlock(bc, 0);
    CHECK(gProbe.retriggers == 2); // both running instances (FX-089)
    bank.silenceAndClear();
    bool reachedSilence = false;
    core::rt::ScopedRealtimeThread rt;
    for (int k = 0; k < 8; ++k)
    {
        b = in;
        bank.processMaster(b.l, b.r, ctx);
        reachedSilence = reachedSilence || (b.l[63] == 0.0f && b.r[63] == 0.0f);
    }
    CHECK(reachedSilence);
    CHECK(gProbe.clears >= 1);
    CHECK(b.l[10] == in.l[10] * 2.0f); // faded back in
    FxMeterFeed feed;
    bank.setMeteredSlot(kMasterSlot);
    b = in;
    bank.processMaster(b.l, b.r, ctx);
    bank.publish(feed);
    CHECK(feed.meteredSlot == kMasterSlot);
    CHECK(feed.outPeak > feed.inPeak);
    CHECK(((feed.runningMask >> kMasterSlot) & 1u) == 1u);
    CHECK(feed.current[kMasterSlot] == 0);
    CHECK(feed.current[kSendSlot] == -1);
}
