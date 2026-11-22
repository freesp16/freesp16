// Steps 8b–13 with the real mixer (FX-012, MIX-023, MIX-031, MIX-050; bus FX never reach a direct output: a reverb-only
// send gives silence on OUT3-4): the assign feed is the strip's post-fader, post-pan block, the send return and the
// Master FX stay in the master, the trim reaches every output but not the resample tap, the input monitor feeds the
// master and the headphones.
#include "tests/unit/routing/RoutingTestUtil.h"

#include "core/util/GainLaw.h"

#include <catch2/catch_test_macros.hpp>

using namespace routingtest;
using engine::OutPort;
using engine::mixer::RoutingParam;

namespace
{
struct Reverb final : engine::mixer::BusInsert // a "reverb": writes its own constant tail into the bus
{
    void process(float* l, float* r) noexcept override
    {
        for (int i = 0; i < kN; ++i)
            l[i] = r[i] = 0.3f;
    }
};
struct Doubler final : engine::mixer::BusInsert // a Master FX that changes the master
{
    void process(float* l, float* r) noexcept override
    {
        for (int i = 0; i < kN; ++i)
        {
            l[i] *= 2.0f;
            r[i] *= 2.0f;
        }
    }
};

void assign(ChainRig& c, int track, int index) { c.stage->set(RoutingParam::OutAssign, static_cast<std::uint16_t>(track), static_cast<float>(index)); }
} // namespace

TEST_CASE("routing.chain: a reverb-only send gives silence on OUT 3-4; Send/Master FX stay in the master (FX-012)",
          "[routing]")
{
    ChainRig c;
    Reverb reverb;
    Doubler doubler;
    c.hooks.sendFx = &reverb;
    c.hooks.masterFx = &doubler;
    engine::mixer::StripParams p;
    p.volume = 0; // fader down, send up: the track only reaches the send bus
    p.send = 127;
    c.mixer->setStrip(0, 0, p, 0);
    c.mixer->prepare(44100.0, kN);
    assign(c, 0, 2); // OUT 3-4
    for (int b = 0; b < 6; ++b)
        c.dc(1);
    CHECK(c.peak(OutPort::Out34) == 0.0f);
    CHECK(c.peak(OutPort::Main) > 0.29f); // the return (0.3) is in the master
    // Fader up: OUT 3-4 = the post-fader track exactly, without the Master FX's doubling or the return.
    p.volume = 100;
    c.mixer->setStrip(0, 0, p, 0);
    for (int b = 0; b < 4; ++b)
        c.dc(1);
    REQUIRE(c.port(OutPort::Out34, 0) != nullptr);
    CHECK(c.port(OutPort::Out34, 0)[9] == 0.5f);
    CHECK(c.port(OutPort::Main, 0)[9] == 0.5f * 2.0f + 0.3f);
}

TEST_CASE("routing.chain: the assign feed is post-pan and post-fader; a muted track feeds nothing (MIX-031)", "[routing]")
{
    ChainRig c;
    engine::mixer::StripParams p;
    p.pan = 32;
    p.volume = 50;
    c.mixer->setStrip(0, 3, p, 0);
    c.mixer->prepare(44100.0, kN);
    assign(c, 3, 3); // OUT 5-6
    for (int b = 0; b < 5; ++b)
        c.dc(1u << 3);
    const float g = core::GainLaw::gain(50);
    REQUIRE(c.port(OutPort::Out56, 0) != nullptr);
    CHECK(c.port(OutPort::Out56, 0)[0] == (0.5f * 0.5f) * g); // pan +32: L = 32/64
    CHECK(c.port(OutPort::Out56, 1)[0] == 0.5f * g);
    p.mute = true;
    c.mixer->setStrip(0, 3, p, 0);
    for (int b = 0; b < 3; ++b)
        c.dc(1u << 3);
    CHECK(c.peak(OutPort::Out56) == 0.0f);
}

TEST_CASE("routing.chain: 8CH — OUT 1-2 carries only its tracks; M + 6CH — the master plus a copy on the pair",
          "[routing]")
{
    ChainRig c;
    assign(c, 0, 2); // track 1 → OUT 3-4
    for (int b = 0; b < 5; ++b)
        c.dc(0b11); // tracks 1 and 2
    CHECK(c.port(OutPort::Main, 0)[0] == 1.0f); // both in the master (M + 6CH)
    CHECK(c.port(OutPort::Out34, 0)[0] == 0.5f);
    c.stage->set(RoutingParam::AssignMode, 0, 1.0f);
    for (int b = 0; b < 5; ++b)
        c.dc(0b11);
    CHECK(c.port(OutPort::Main, 0)[0] == 0.5f); // only track 2 ("--" = OUT 1-2 in 8CH)
    CHECK(c.port(OutPort::Out34, 0)[0] == 0.5f);
    c.stage->set(RoutingParam::AssignMode, 1, 0.0f); // scene 2: not the current scene
    c.dc(0b11);
    CHECK(c.stage->router().mode() == AssignMode::EightChannel);
    c.stage->setScene(1); // a scene switch applies the scene's routing (scene 2 = M + 6CH)
    for (int b = 0; b < 5; ++b)
        c.dc(0b11);
    CHECK(c.stage->router().mode() == AssignMode::MasterPlus6);
    CHECK(c.peak(OutPort::Out34) == 0.0f); // scene 2's track 1 is "--"
}

TEST_CASE("routing.chain: the output trim reaches every output incl. headphones, not the resample tap (MIX-023)",
          "[routing]")
{
    ChainRig c;
    assign(c, 0, 2);
    c.stage->setHeadphoneVolume(127.0f);
    for (int b = 0; b < 5; ++b)
        c.dc(1);
    CHECK(c.port(OutPort::Main, 0) == c.b->master.l); // unity trim: the master itself
    CHECK(c.port(OutPort::Phones, 0)[0] == 0.5f);
    c.stage->setOutputTrim(512.0f);
    for (int b = 0; b < 4; ++b)
        c.dc(1);
    const float t = engine::mixer::outputTrimGain(512);
    CHECK(c.port(OutPort::Main, 0)[0] == 0.5f * t);
    CHECK(c.port(OutPort::Out34, 0)[0] == 0.5f * t);
    CHECK(c.port(OutPort::Phones, 0)[0] == 0.5f * t);
    CHECK(c.stage->resampleTap(0)[0] == 0.5f);
    CHECK(c.port(OutPort::FilterSend, 0) == nullptr); // existing semantics: silent unless the AFS slot writes it
}

TEST_CASE("routing.chain: absent ports are not rendered; the headphones skip their work without the port", "[routing]")
{
    ChainRig c;
    c.stage->setPresent(0x01); // Main only
    assign(c, 0, 2);
    for (int b = 0; b < 5; ++b)
        c.dc(1);
    CHECK(c.port(OutPort::Out34, 0) == nullptr);
    CHECK(c.port(OutPort::Phones, 0) == nullptr);
    CHECK(c.port(OutPort::Main, 0)[0] == 0.5f);
}

TEST_CASE("routing.chain: the input monitor feeds the master (before the fader) and the headphones (MIX-050)", "[routing]")
{
    ChainRig c;
    engine::stages::StereoBlock in {};
    for (int i = 0; i < kN; ++i)
        in.l[i] = in.r[i] = 0.25f;
    c.b->inputL = in.l;
    c.b->inputR = in.r;
    c.stage->setHeadphoneVolume(127.0f);
    c.stage->set(RoutingParam::Monitor, 0, 1.0f);
    for (int b = 0; b < 4; ++b)
        c.dc(0);
    CHECK(c.port(OutPort::Main, 0)[0] == 0.25f);   // master = the monitor (no track plays)
    CHECK(c.port(OutPort::Phones, 0)[0] == 0.25f); // cue MASTER: once, not twice (the gate is closed)
    c.mixer->setGlobal(engine::mixer::GlobalParam::MasterVolume, 50.0f);
    for (int b = 0; b < 4; ++b)
        c.dc(0);
    CHECK(c.port(OutPort::Main, 0)[0] == 0.25f * core::GainLaw::gain(50));
    c.stage->set(RoutingParam::HeadphoneCue, 0, 2.0f); // OUT 3-4 (silent) + the monitor at unity
    for (int b = 0; b < 5; ++b)
        c.dc(0);
    CHECK(c.port(OutPort::Phones, 0)[0] == 0.25f);
    c.stage->set(RoutingParam::MonitorLevel, 0, 0.0f);
    for (int b = 0; b < 4; ++b)
        c.dc(0);
    CHECK(c.port(OutPort::Phones, 0)[0] == 0.0f);
}

TEST_CASE("routing.chain: Thru BYPASS is kept per scene and track and reported once per change", "[routing]")
{
    ChainRig c;
    CHECK(c.stage->thruBypassChanged());
    CHECK_FALSE(c.stage->thruBypassChanged());
    c.stage->set(RoutingParam::ThruBypass, (1 << 4) | 2, 1.0f); // scene 2, track 3: not current
    CHECK(c.stage->thruBypassMask() == 0);
    CHECK_FALSE(c.stage->thruBypassChanged());
    c.stage->set(RoutingParam::ThruBypass, 5, 1.0f);
    CHECK(c.stage->thruBypassMask() == 1u << 5);
    CHECK(c.stage->thruBypassChanged());
    c.stage->setScene(1);
    CHECK(c.stage->thruBypassMask() == 1u << 2);
    CHECK(c.stage->thruBypassChanged());
}
