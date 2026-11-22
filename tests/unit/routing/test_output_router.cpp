// engine::mixer::OutputRouter (MIX-030…033): M + 6CH keeps an assigned track in the
// master and adds it to its pair, 8CH sends it only to its pair, mono = (L + R) / 2 on one channel, mode and
// assignment changes are exactly 128 + 128-sample dips, absent pairs fold into Main, mute-all fades over 64 samples.
#include "tests/unit/routing/RoutingTestUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace routingtest;
using engine::mixer::OutputRouter;

TEST_CASE("routing.router: M + 6CH with nothing assigned plays the master pointer on OUT 1-2 (no copy)", "[routing]")
{
    RouterRig r;
    r.setMaster(0.25f, -0.5f);
    r.block();
    CHECK(r.out.ch[0] == r.master.l);
    CHECK(r.out.ch[1] == r.master.r);
    for (int c = 2; c < 8; ++c)
        CHECK(r.out.ch[static_cast<std::size_t>(c)] == nullptr);
    CHECK(r.router.resampleTap(0) == r.master.l);
    CHECK(r.router.resampleTap(1) == r.master.r);
}

TEST_CASE("routing.router: M + 6CH adds an assigned track to its pair; OUT1-2, 1 and 2 are master only", "[routing]")
{
    RouterRig r;
    r.setMaster(0.1f, 0.1f);
    r.setTrack(0, 0.5f, -0.25f);
    r.setTrack(1, 0.3f, 0.1f);
    r.setTrack(2, 0.7f, 0.7f);
    r.router.setAssign(0, pair(1)); // OUT 3-4
    r.router.setAssign(1, mono(5)); // channel 6 (= OUT 5-6 right)
    r.router.setAssign(2, pair(0)); // OUT 1-2: not selectable in M + 6CH
    for (int b = 0; b < 4; ++b)     // the assignment fades (2 + 2 blocks) are over
        r.block();
    r.block();
    CHECK(r.out.ch[0] == r.master.l); // the master is untouched: the track stays in it through the mixer
    CHECK(r.at(2, 10) == 0.5f);
    CHECK(r.at(3, 10) == -0.25f);
    CHECK(r.at(4, 10) == 0.0f);                 // OUT 5-6 left: the pair is live, its other side silent
    CHECK(r.at(5, 10) == (0.3f + 0.1f) * 0.5f); // mono = (L + R) / 2
    CHECK(r.out.ch[6] == nullptr);
    CHECK(r.router.destinations(2) == 0);
    r.router.setAssign(3, mono(0)); // channel 1: master only in M + 6CH
    CHECK(r.router.destinations(3) == 0);
}

TEST_CASE("routing.router: 8CH sends every track to exactly one output; OUT 1-2 carries no master", "[routing]")
{
    RouterRig r;
    r.router.setMode(AssignMode::EightChannel);
    for (int b = 0; b < 4; ++b)
        r.block();
    REQUIRE(r.router.mode() == AssignMode::EightChannel);
    r.setMaster(0.9f, 0.9f);
    r.valid = 0b111;
    r.setTrack(0, 0.5f, 0.25f); // "--" → OUT 1-2 (our default)
    r.setTrack(1, 0.125f, 0.0625f);
    r.setTrack(2, 0.2f, 0.4f);
    r.router.setAssign(1, pair(1));
    r.router.setAssign(2, mono(0)); // channel 1 = OUT 1-2 left
    for (int b = 0; b < 4; ++b)
        r.block();
    r.block();
    CHECK(r.at(0, 5) == 0.5f + (0.2f + 0.4f) * 0.5f);
    CHECK(r.at(1, 5) == 0.25f);
    CHECK(r.at(2, 5) == 0.125f);
    CHECK(r.at(3, 5) == 0.0625f);
    CHECK(r.out.ch[4] == nullptr);
    CHECK(r.router.destinations(0) == 0x11);
    CHECK(r.router.destinations(1) == 0x22);
    CHECK(r.router.destinations(2) == 0x01);
    r.valid = 0; // silent tracks feed nothing (and the master never does in 8CH)
    r.block();
    CHECK(r.out.ch[0] == nullptr);
    CHECK(r.out.ch[2] == nullptr);
}

TEST_CASE("routing.router: a mode change is exactly a 128-sample fade-out, the switch, a 128-sample fade-in (MIX-033)",
          "[routing]")
{
    RouterRig r;
    r.setMaster(1.0f, 1.0f);
    r.setTrack(0, 1.0f, 1.0f);
    r.valid = 1;
    r.router.setAssign(0, pair(1));
    for (int b = 0; b < 4; ++b)
        r.block();
    r.block();
    REQUIRE(r.at(2, 0) == 1.0f);
    r.router.setMode(AssignMode::EightChannel);
    std::vector<float> main, out34;
    for (int b = 0; b < 5; ++b)
    {
        r.block();
        for (int i = 0; i < kN; ++i)
        {
            main.push_back(r.at(0, i));
            out34.push_back(r.at(2, i));
        }
    }
    for (int k = 0; k < 128; ++k)
    {
        INFO(k);
        // the research Selector (F212): 1 … 1/128 out, 0 … 127/128 in
        CHECK(main[static_cast<std::size_t>(k)] == static_cast<float>(128 - k) / 128.0f); // the master fades out
        CHECK(out34[static_cast<std::size_t>(k)] == static_cast<float>(128 - k) / 128.0f);
        CHECK(main[static_cast<std::size_t>(128 + k)] == 0.0f); // 8CH: OUT 1-2 has no track assigned to it
        CHECK(out34[static_cast<std::size_t>(128 + k)] == static_cast<float>(k) / 128.0f);
    }
    for (std::size_t i = 256; i < out34.size(); ++i)
        CHECK(out34[i] == 1.0f);
    CHECK(r.router.mode() == AssignMode::EightChannel);
    CHECK_FALSE(r.router.fading());
}

TEST_CASE("routing.router: re-selecting the mode during the fade-out fades back in (no stuck silence)", "[routing]")
{
    RouterRig r;
    r.setMaster(1.0f, 1.0f);
    r.router.setMode(AssignMode::EightChannel);
    r.block(); // 64 frames of fade-out
    CHECK(r.at(0, 63) == 65.0f / 128.0f);
    r.router.setMode(AssignMode::MasterPlus6); // back: fades in from the gain reached (0.5) over 128 frames
    r.block();
    CHECK(r.at(0, 0) == 0.5f);
    CHECK(r.at(0, 63) == 0.5f + 63.0f * (0.5f / 128.0f));
    CHECK_FALSE(r.router.stuck());
    r.block();
    CHECK(r.at(0, 63) == 0.5f + 127.0f * (0.5f / 128.0f));
    r.block();
    CHECK(r.out.ch[0] == r.master.l);
    CHECK(r.router.mode() == AssignMode::MasterPlus6);
    CHECK_FALSE(r.router.fading());
}

TEST_CASE("routing.router: reference profile — re-selecting the mode during the fade-out stays silent (F209/F212)",
          "[routing]")
{
    RouterRig r;
    r.router.setReferenceQuirk(true);
    r.setMaster(1.0f, 1.0f);
    r.router.setMode(AssignMode::EightChannel);
    r.block();
    r.router.setMode(AssignMode::MasterPlus6); // the reference Selector: the fade-out goes on to 0 and stays there
    r.block();
    CHECK(r.at(0, 0) == 0.5f);
    CHECK(r.at(0, 63) == 1.0f / 128.0f);
    CHECK(r.router.stuck());
    for (int b = 0; b < 4; ++b)
    {
        r.block();
        CHECK(r.out.ch[0] == nullptr);
    }
    CHECK(r.router.mode() == AssignMode::MasterPlus6);
    r.router.setMode(AssignMode::EightChannel); // the next change: the new selection fades in from 0
    r.setTrack(0, 1.0f, 1.0f);
    r.valid = 1;
    r.block();
    CHECK(r.router.mode() == AssignMode::EightChannel);
    CHECK(r.at(0, 0) == 0.0f);
    CHECK(r.at(0, 1) == 1.0f / 128.0f);
    CHECK_FALSE(r.router.stuck());
}

TEST_CASE("routing.router: an assignment change fades the old output out and the new one in (128 + 128)", "[routing]")
{
    RouterRig r;
    r.setTrack(0, 1.0f, 1.0f);
    r.valid = 1;
    r.router.setAssign(0, pair(1));
    for (int b = 0; b < 4; ++b)
        r.block();
    r.router.setAssign(0, pair(2));
    std::vector<float> a, c;
    for (int b = 0; b < 4; ++b)
    {
        r.block();
        for (int i = 0; i < kN; ++i)
        {
            a.push_back(r.at(2, i));
            c.push_back(r.at(4, i));
        }
    }
    for (int k = 0; k < 128; ++k)
    {
        CHECK(a[static_cast<std::size_t>(k)] == static_cast<float>(128 - k) / 128.0f);
        CHECK(c[static_cast<std::size_t>(k)] == 0.0f);
        CHECK(a[static_cast<std::size_t>(128 + k)] == 0.0f);
        CHECK(c[static_cast<std::size_t>(128 + k)] == static_cast<float>(k) / 128.0f);
    }
    CHECK(r.router.assign(0) == pair(2));
}

TEST_CASE("routing.router: absent pairs fold into Main (MIX-032)", "[routing]")
{
    RouterRig r;
    r.router.setPresent(0x1); // a stereo interface: OUT 1-2 only
    r.setMaster(0.1f, 0.1f);
    r.setTrack(0, 0.5f, 0.5f);
    r.valid = 1;
    r.router.setAssign(0, pair(1));
    for (int b = 0; b < 5; ++b)
        r.block();
    CHECK(r.out.ch[0] == r.master.l); // M + 6CH: the track is in the master already
    CHECK(r.out.ch[2] == nullptr);
    r.router.setMode(AssignMode::EightChannel);
    for (int b = 0; b < 5; ++b)
        r.block();
    CHECK(r.at(0, 0) == 0.5f); // 8CH: its pair is absent, so it plays on OUT 1-2
    CHECK(r.out.ch[2] == nullptr);
    r.router.setAssign(0, mono(5)); // channel 6 → OUT 1-2 right
    for (int b = 0; b < 5; ++b)
        r.block();
    CHECK(r.at(0, 0) == 0.0f);
    CHECK(r.at(1, 0) == 0.5f);
}

TEST_CASE("routing.router: mute-all fades every output over 64 samples and back", "[routing]")
{
    RouterRig r;
    bool silent = true;
    r.block();
    CHECK(r.router.muteAllGains(silent) == nullptr);
    CHECK_FALSE(silent);
    r.router.setMuteAll(true);
    r.block();
    const float* g = r.router.muteAllGains(silent);
    REQUIRE(g != nullptr);
    CHECK_FALSE(silent);
    for (int k = 0; k < kN; ++k)
        CHECK(g[k] == static_cast<float>(64 - k) / 64.0f); // the research BulkMute: 1 … 1/64, then closed
    r.block();
    CHECK(r.router.muteAllGains(silent) == nullptr);
    CHECK(silent);
    r.router.setMuteAll(false);
    r.block();
    g = r.router.muteAllGains(silent);
    REQUIRE(g != nullptr);
    for (int k = 0; k < kN; ++k)
        CHECK(g[k] == static_cast<float>(k) / 64.0f); // 0 … 63/64, then open
    r.block();
    CHECK(r.router.muteAllGains(silent) == nullptr);
    CHECK_FALSE(silent);
    CHECK(r.router.resampleTap(0) == r.master.l); // the tap is never muted (our choice)
}

TEST_CASE("routing.router: OUT ASSIGN indices follow core::model::OutputAssign", "[routing]")
{
    using engine::mixer::outAssignFromIndex;
    CHECK(outAssignFromIndex(0) == OutAssign {});
    CHECK(outAssignFromIndex(1) == pair(0));
    CHECK(outAssignFromIndex(4) == pair(3));
    CHECK(outAssignFromIndex(5) == mono(0));
    CHECK(outAssignFromIndex(12) == mono(7));
    CHECK(outAssignFromIndex(13) == OutAssign {});
    CHECK(outAssignFromIndex(-1) == OutAssign {});
}
