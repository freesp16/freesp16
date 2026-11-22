// engine::mixer::HeadphoneRouter and InputMonitor (MIX-040…042, MIX-050, UI-323; research
// signal-flow.md "Main output and headphones" F209–F213): cue sources, 128 + 128 cue fades, the reference re-select
// quirk only in the reference profile, the per-mode correction of invalid cues, the monitor gates, the level law.
#include "tests/unit/routing/RoutingTestUtil.h"

#include "core/util/GainLaw.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace routingtest;
using engine::mixer::Cue;
using engine::mixer::HeadphoneRouter;
using engine::mixer::MonitorBuses;

namespace
{
struct HpRig
{
    HeadphoneRouter hp;
    engine::stages::StereoBlock master {}, aux {}, pairs[4] {};
    engine::mixer::OutputBuffers out {};
    MonitorBuses mon {};
    alignas(16) float l[kN] {}, r[kN] {};

    HpRig()
    {
        hp.setLevel(127.0f); // unity, so the checks read the sources directly
        hp.prepare(44100.0);
        for (int i = 0; i < kN; ++i)
        {
            master.l[i] = 0.5f;
            master.r[i] = -0.5f;
            for (int p = 0; p < 4; ++p)
            {
                pairs[p].l[i] = 0.1f * static_cast<float>(p + 1);
                pairs[p].r[i] = -0.01f * static_cast<float>(p + 1);
            }
            pairs[0].l[i] = master.l[i]; // M + 6CH: the router's OUT 1-2 is the master (MASTER plays it, F209)
            pairs[0].r[i] = master.r[i];
        }
        for (int p = 0; p < 4; ++p)
        {
            out.ch[static_cast<std::size_t>(2 * p)] = pairs[p].l;
            out.ch[static_cast<std::size_t>(2 * p + 1)] = pairs[p].r;
        }
    }
    void block()
    {
        const float* m[2] = {master.l, master.r};
        hp.process(out, m, mon, l, r);
    }
    void settle()
    {
        for (int b = 0; b < 5; ++b)
            block();
    }
};
} // namespace

TEST_CASE("routing.headphones: MASTER, a pair, a mono channel duplicated to both ears (MIX-040)", "[routing]")
{
    HpRig h;
    h.block();
    CHECK(h.l[0] == 0.5f);
    CHECK(h.r[0] == -0.5f);
    h.hp.setCue(static_cast<int>(Cue::Out56));
    h.settle();
    CHECK(h.l[3] == h.pairs[2].l[3]);
    CHECK(h.r[3] == h.pairs[2].r[3]);
    h.hp.setCue(static_cast<int>(Cue::Mono4)); // channel 4 = OUT 3-4 right
    h.settle();
    CHECK(h.l[7] == h.pairs[1].r[7]);
    CHECK(h.r[7] == h.pairs[1].r[7]);
    h.out.ch[4] = h.out.ch[5] = nullptr; // a silent pair
    h.hp.setCue(static_cast<int>(Cue::Out56));
    h.settle();
    CHECK(h.l[0] == 0.0f);
    CHECK(h.r[0] == 0.0f);
}

TEST_CASE("routing.headphones: a cue change fades out over 128 samples and in over 128 (MIX-042)", "[routing]")
{
    HpRig h;
    h.block();
    h.hp.setCue(static_cast<int>(Cue::Out34));
    std::vector<float> l;
    for (int b = 0; b < 5; ++b)
    {
        h.block();
        l.insert(l.end(), h.l, h.l + kN);
    }
    for (int k = 0; k < 128; ++k) // the research Selector (F209): 1 … 1/128 out, 0 … 127/128 in
    {
        INFO(k);
        CHECK(l[static_cast<std::size_t>(k)] == 0.5f * (static_cast<float>(128 - k) / 128.0f));
        CHECK(l[static_cast<std::size_t>(128 + k)] == 0.2f * (static_cast<float>(k) / 128.0f));
    }
    CHECK(l[300] == 0.2f);
    CHECK(h.hp.audible() == Cue::Out34);
}

TEST_CASE("routing.headphones: re-selecting the source that fades out — app fades back in, reference stays at 0 (F209)",
          "[routing]")
{
    SECTION("app profile")
    {
        HpRig h;
        h.hp.setCue(static_cast<int>(Cue::Out34));
        h.block(); // fade-out of MASTER, 64 frames done (gain 0.5 reached)
        h.hp.setCue(static_cast<int>(Cue::Master));
        h.block(); // back in from 0.5 over 128 frames
        CHECK(h.l[0] == 0.5f * 0.5f);
        CHECK(h.l[63] == 0.5f * (0.5f + 63.0f * (0.5f / 128.0f)));
        h.block();
        h.block();
        CHECK(h.l[10] == 0.5f);
        CHECK_FALSE(h.hp.stuck());
        CHECK_FALSE(h.hp.fading());
    }
    SECTION("reference profile")
    {
        HpRig h;
        h.hp.setReferenceQuirk(true);
        h.hp.setCue(static_cast<int>(Cue::Out34));
        h.block();
        h.hp.setCue(static_cast<int>(Cue::Master));
        CHECK_FALSE(h.hp.stuck()); // the fade-out goes on (the reference Selector keeps its ramp)
        h.block();
        CHECK(h.l[0] == 0.5f * 0.5f);
        CHECK(h.l[63] == 0.5f * (1.0f / 128.0f));
        CHECK(h.hp.stuck());
        for (int b = 0; b < 6; ++b)
        {
            h.block();
            CHECK(h.l[0] == 0.0f);
            CHECK(h.l[63] == 0.0f);
        }
        h.hp.setCue(static_cast<int>(Cue::Out78)); // the next change: the new source fades in from 0
        h.block();
        CHECK_FALSE(h.hp.stuck());
        CHECK(h.l[0] == 0.0f);
        CHECK(h.l[1] == 0.4f * (1.0f / 128.0f));
        h.settle();
        CHECK(h.l[0] == 0.4f);
    }
}

TEST_CASE("routing.headphones: a change during a fade-in fades out over 128 samples from the gain reached", "[routing]")
{
    HpRig h;
    h.hp.setCue(static_cast<int>(Cue::Out34));
    for (int b = 0; b < 3; ++b) // 128 frames out, then 64 of OUT 3-4 in (gain 0.5 reached)
        h.block();
    h.hp.setCue(static_cast<int>(Cue::Out56));
    std::vector<float> l;
    for (int b = 0; b < 3; ++b)
    {
        h.block();
        l.insert(l.end(), h.l, h.l + kN);
    }
    for (int k = 0; k < 128; ++k)
        CHECK(l[static_cast<std::size_t>(k)] == 0.2f * (0.5f * static_cast<float>(128 - k) / 128.0f));
    CHECK(l[128] == 0.0f); // OUT 5-6 starts its fade-in
    CHECK(l[129] == 0.3f * (1.0f / 128.0f));
    CHECK(h.hp.audible() == Cue::Out56);
}

TEST_CASE("routing.headphones: invalid cues are corrected per mode and come back with it (MIX-040)", "[routing]")
{
    using engine::mixer::effectiveCue;
    CHECK(effectiveCue(Cue::Out12, AssignMode::MasterPlus6) == Cue::Master);
    CHECK(effectiveCue(Cue::Mono1, AssignMode::MasterPlus6) == Cue::Master);
    CHECK(effectiveCue(Cue::Mono2, AssignMode::MasterPlus6) == Cue::Master);
    CHECK(effectiveCue(Cue::Mono3, AssignMode::MasterPlus6) == Cue::Mono3);
    CHECK(effectiveCue(Cue::Master, AssignMode::EightChannel) == Cue::Out12);
    CHECK(effectiveCue(Cue::Mono1, AssignMode::EightChannel) == Cue::Mono1);

    HpRig h;
    h.hp.setCue(static_cast<int>(Cue::Out12)); // M + 6CH: plays MASTER
    h.settle();
    CHECK(h.hp.audible() == Cue::Master);
    CHECK(h.l[0] == 0.5f);
    h.hp.setMode(AssignMode::EightChannel);
    CHECK(h.hp.audible() == Cue::Out12); // MASTER and OUT 1-2 are one selector input: no headphone fade (F209)
    CHECK_FALSE(h.hp.fading());
    for (int i = 0; i < kN; ++i)
        h.pairs[0].l[i] = 0.1f; // 8CH: OUT 1-2 carries its assigned tracks
    h.block();
    CHECK(h.l[0] == 0.1f);
    CHECK(h.hp.requested() == Cue::Out12);
}

TEST_CASE("routing.headphones: preview/metronome and the input monitor are added at unity behind 44-sample gates",
          "[routing]")
{
    HpRig h;
    engine::stages::StereoBlock aux {}, in {};
    for (int i = 0; i < kN; ++i)
    {
        aux.l[i] = aux.r[i] = 0.25f;
        in.l[i] = in.r[i] = 0.125f;
    }
    h.mon = {aux.l, aux.r, in.l, in.r};
    h.block();
    CHECK(h.l[0] == 0.5f); // MASTER: the gate is closed (the master carries them already in the app)
    h.hp.setCue(static_cast<int>(Cue::Out34));
    h.block();
    h.block();
    h.block(); // fade-in of OUT 3-4, the gate opens from its first frame (app: the audible cue decides)
    const float inv = static_cast<float>(1.0 / 44.0);
    float acc = 0.0f; // the research MuteSmoother(44): 0, inv, 2 inv … accumulated, open after 44 frames
    for (int i = 0; i < kN; ++i)
    {
        const float g = i < 44 ? acc : 1.0f;
        acc = acc + inv;
        const float sel = 0.2f * (static_cast<float>(i) / 128.0f);
        const float x = sel + 0.25f * g;
        CHECK(h.l[i] == Catch::Approx(x + 0.125f * g).margin(1e-6)); // the test's own arithmetic may fuse
    }
    h.settle();
    CHECK(h.l[5] == 0.2f + 0.25f + 0.125f);
}

TEST_CASE("routing.headphones: the HEADPHONES level is the fader law x 0.5 with a 100-sample ramp (MIX-041)", "[routing]")
{
    HpRig h;
    h.hp.setLevel(100.0f);
    h.block();
    CHECK(h.hp.level() < 1.0f);
    for (int b = 0; b < 3; ++b)
        h.block();
    CHECK(h.hp.level() == core::GainLaw::gain(100) * 0.5f);
    CHECK(h.l[0] == 0.5f * 0.5f);
    h.hp.setLevel(0.0f);
    h.settle();
    CHECK(h.l[0] == 0.0f);
}

TEST_CASE("routing.monitor: off by default; on fades in over 128 samples; level 100 = the input itself", "[routing]")
{
    engine::mixer::InputMonitor m;
    m.prepare(44100.0);
    engine::stages::StereoBlock in {};
    for (int i = 0; i < kN; ++i)
        in.l[i] = in.r[i] = 0.5f;
    MonitorBuses mb {};
    m.process(in.l, in.r, mb);
    CHECK(mb.inputL == nullptr);
    m.setOn(true);
    std::vector<float> x;
    for (int b = 0; b < 2; ++b)
    {
        m.process(in.l, in.r, mb);
        REQUIRE(mb.inputL != nullptr);
        x.insert(x.end(), mb.inputL, mb.inputL + kN);
    }
    CHECK(x[0] == 0.0f);          // the mute ramp's first gain (MuteSmoother)
    CHECK(x[127] < 0.5f);
    m.process(in.l, in.r, mb);    // the ramp lands (MuteRamp: the block after the 128 frames is still a buffer of 1)
    CHECK(mb.inputL[0] == 0.5f);
    m.process(in.l, in.r, mb);
    CHECK(mb.inputL == in.l); // unity: no copy
    m.setLevel(50.0f);
    for (int b = 0; b < 3; ++b)
        m.process(in.l, in.r, mb);
    CHECK(mb.inputL[7] == 0.5f * core::GainLaw::gain(50));
    m.process(nullptr, nullptr, mb); // no input
    CHECK(mb.inputL == nullptr);
    m.setOn(false);
    for (int b = 0; b < 4; ++b)
        m.process(in.l, in.r, mb);
    CHECK(mb.inputL == nullptr);
    CHECK_FALSE(m.on());
}
