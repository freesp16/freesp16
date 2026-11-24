// The analog-filter slot around its processors (AFS-001…003, AFS-005, AFS-006, AFS-011, AFS-012; research
// signal-flow.md F107, F187): the drive law and its 100-sample ramp, OVER threshold and hold, the 128/128 dip on every
// route / order / ACTIVE / processor change (exact shape), bypass bit-transparency (NFR-030), the pre/post-FX points
// around the track's inserts, the desktop fallback to the model, and the external loop's ping on a loopback.
#include "tests/unit/afs/AfsTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <deque>

using namespace afstest;
using engine::afs::Processor;

namespace
{
// An insert that records the block it receives and then scales it (FX1/FX2 stand-in).
struct RecordingInsert final : engine::mixer::BusInsert
{
    std::array<float, N> seenL {}, seenR {};
    float gain = 0.5f;
    void process(float* l, float* r) noexcept override
    {
        std::copy(l, l + N, seenL.begin());
        std::copy(r, r + N, seenR.begin());
        for (int i = 0; i < N; ++i)
        {
            l[i] *= gain;
            r[i] *= gain;
        }
    }
};

// The external loop with the device's ports, ACTIVE on, on the master.
void externalOnMaster(SlotRig& rig)
{
    rig.set(AfsField::Active, 1);
    rig.set(AfsField::Processor, 1);
    rig.settle();
    REQUIRE(rig.slot.processorInUse() == Processor::External);
}
} // namespace

TEST_CASE("afs.drive: gain 1 + 3·(i/1023)², 0 … +12.04 dB, with the 100-sample linear ramp (F107)", "[afs][drive]")
{
    CHECK(engine::afs::driveGain(0) == 1.0f);
    CHECK(engine::afs::driveGain(1023) == 4.0f);
    CHECK(std::fabs(engine::afs::driveGain(512) - (1.0f + 3.0f * (512.0f / 1023.0f) * (512.0f / 1023.0f))) < 1e-7f);
    CHECK(std::fabs(20.0 * std::log10(static_cast<double>(engine::afs::driveGain(1023))) - 12.0412) < 1e-3);

    SlotRig rig(true);
    externalOnMaster(rig); // the send carries the driven signal exactly
    rig.set(AfsField::Drive, 1023);
    float cur = 1.0f;
    const float step = (4.0f - 1.0f) / 100.0f;
    int left = 100;
    for (int b = 0; b < 3; ++b)
    {
        std::array<float, N> l, r;
        l.fill(0.25f);
        r.fill(-0.5f);
        rig.master(l.data(), r.data());
        for (int i = 0; i < N; ++i)
        {
            if (left > 0)
            {
                cur += step;
                if (--left == 0)
                    cur = 4.0f;
            }
            INFO("block " << b << " frame " << i);
            CHECK(rig.sendL[static_cast<std::size_t>(i)] == 0.25f * cur);
            CHECK(rig.sendR[static_cast<std::size_t>(i)] == -0.5f * cur);
        }
    }
    CHECK(rig.sendL[N - 1] == 1.0f); // settled at ×4
}

TEST_CASE("afs.over: the lamp lights above +5.1 dBFS on the drive output and holds 0.5 s (AFS-011)", "[afs][over]")
{
    SlotRig rig;
    rig.set(AfsField::Active, 1);
    rig.settle();
    auto blockAt = [&](float peak) {
        std::array<float, N> l {}, r {};
        l[10] = peak;
        rig.master(l.data(), r.data());
    };
    blockAt(1.79f);
    CHECK_FALSE(rig.slot.over());
    blockAt(1.80f);
    CHECK(rig.slot.over());
    int lit = 0;
    for (int b = 0; b < 400; ++b)
    {
        blockAt(0.0f);
        lit += rig.slot.over() ? 1 : 0;
    }
    CHECK(lit * N >= 22050 - N); // ≈ 0.5 s at 44.1 kHz
    CHECK(lit * N <= 22050 + N);
    CHECK_FALSE(rig.slot.over());

    // Drive pushes a 0 dBFS peak over the threshold: ×4 = +12 dBFS.
    rig.set(AfsField::Drive, 1023);
    for (int b = 0; b < 3; ++b)
        blockAt(1.0f);
    CHECK(rig.slot.over());
    CHECK(rig.slot.overLevel() == 4.0f);
}

TEST_CASE("afs.dip: route, order, ACTIVE and processor changes dip 128 samples down and 128 up (AFS-006)", "[afs][dip]")
{
    const auto expectDip = [](const std::vector<float>& g) {
        REQUIRE(g.size() >= 320);
        for (int i = 0; i < 128; ++i)
            CHECK(g[static_cast<std::size_t>(i)] == static_cast<float>(127 - i) / 128.0f);
        for (int i = 0; i < 128; ++i)
            CHECK(g[static_cast<std::size_t>(128 + i)] == static_cast<float>(i + 1) / 128.0f);
        for (std::size_t i = 256; i < g.size(); ++i)
            CHECK(g[i] == 1.0f);
    };

    SECTION("route MASTER → TRACK 1, ACTIVE off: the master path dips")
    {
        SlotRig rig;
        rig.set(AfsField::InsertTrack, 1);
        std::vector<float> g;
        for (int b = 0; b < 6; ++b)
        {
            std::array<float, N> l, r;
            l.fill(1.0f);
            r.fill(1.0f);
            rig.master(l.data(), r.data());
            g.insert(g.end(), l.begin(), l.end());
            CHECK(std::equal(l.begin(), l.end(), r.begin()));
        }
        expectDip(g);
        CHECK(rig.slot.route() == Route::TrackPreFx);
        CHECK(rig.slot.track() == 0);
    }
    SECTION("insert order PRE → POST on track 5: the track's path after its inserts dips")
    {
        SlotRig rig;
        rig.set(AfsField::InsertTrack, 5);
        rig.settle();
        RecordingInsert fx;
        fx.gain = 1.0f;
        rig.set(AfsField::InsertOrder, 1);
        std::vector<float> g;
        for (int b = 0; b < 6; ++b)
        {
            std::array<float, N> l, r;
            l.fill(1.0f);
            r.fill(1.0f);
            rig.track(4, l.data(), r.data(), &fx);
            g.insert(g.end(), l.begin(), l.end());
        }
        expectDip(g);
        CHECK(rig.slot.route() == Route::TrackPostFx);
    }
    SECTION("ACTIVE and the processor dip; the knobs do not")
    {
        SlotRig rig(true);
        rig.set(AfsField::Drive, 0);
        rig.set(AfsField::LpCutoff, 500);
        rig.set(AfsField::Processor, 0); // MODEL (the default is EXTERNAL)
        rig.settle();
        CHECK_FALSE(rig.slot.dipping());
        rig.set(AfsField::Active, 1);
        CHECK(rig.slot.dipping());
        std::array<float, N> l {}, r {};
        for (int b = 0; b < 5; ++b)
            rig.master(l.data(), r.data());
        CHECK_FALSE(rig.slot.dipping());
        CHECK(rig.slot.active());
        rig.set(AfsField::Processor, 1);
        CHECK(rig.slot.dipping());
        for (int b = 0; b < 5; ++b)
            rig.master(l.data(), r.data());
        CHECK(rig.slot.processorInUse() == Processor::External);
        rig.set(AfsField::Resonance, 900);
        rig.set(AfsField::HpCutoff, 200);
        rig.set(AfsField::Slope, 1);
        rig.set(AfsField::InsertOrder, 1); // the order alone does not move the master point
        CHECK_FALSE(rig.slot.dipping());
    }
}

TEST_CASE("afs.bypass: ACTIVE off passes every point bit for bit (AFS-012, NFR-030)", "[afs][bypass]")
{
    SlotRig rig;
    rig.set(AfsField::Drive, 0); // DRIVE stays on the main output with ACTIVE off (F107): unity at 0
    rig.set(AfsField::LpCutoff, 100);
    rig.set(AfsField::Resonance, 1023);
    rig.set(AfsField::HpCutoff, 900);
    rig.settle();
    const auto inL = noise(16 * N, 1), inR = noise(16 * N, 2);
    auto l = inL, r = inR;
    runMaster(rig, l, r);
    CHECK(sameBits(l, inL));
    CHECK(sameBits(r, inR));
    CHECK(rig.slot.latencySamples() == 0);
    CHECK_FALSE(rig.slot.over());

    // A track route with ACTIVE off: the insert still runs, nothing else touches the block.
    rig.set(AfsField::InsertTrack, 3);
    rig.settle();
    RecordingInsert fx;
    for (int b = 0; b < 4; ++b)
    {
        std::array<float, N> bl, br;
        std::copy(inL.begin() + b * N, inL.begin() + (b + 1) * N, bl.begin());
        std::copy(inR.begin() + b * N, inR.begin() + (b + 1) * N, br.begin());
        rig.track(2, bl.data(), br.data(), &fx);
        for (int i = 0; i < N; ++i)
            CHECK(bl[static_cast<std::size_t>(i)] == inL[static_cast<std::size_t>(b * N + i)] * 0.5f);
    }
}

TEST_CASE("afs.points: PRE FX sends the source and returns into the inserts; POST FX sends the inserts' output (AFS-002)",
          "[afs][route]")
{
    for (int order : {0, 1})
    {
        SlotRig rig(true);
        rig.set(AfsField::InsertTrack, 8);
        rig.set(AfsField::InsertOrder, static_cast<float>(order));
        rig.set(AfsField::Active, 1);
        rig.set(AfsField::Processor, 1);
        rig.settle();
        REQUIRE(rig.slot.processorInUse() == Processor::External);
        RecordingInsert fx;
        std::array<float, N> pattern;
        for (int i = 0; i < N; ++i)
            pattern[static_cast<std::size_t>(i)] = 0.01f * static_cast<float>(i);
        const auto src = noise(N, 7);
        for (int b = 0; b < 3; ++b)
        {
            std::array<float, N> l, r;
            std::copy(src.begin(), src.end(), l.begin());
            std::copy(src.begin(), src.end(), r.begin());
            rig.track(7, l.data(), r.data(), &fx, pattern.data(), pattern.data());
            if (b == 0)
                continue; // the return arrives one block late
            INFO("order " << order << " block " << b);
            for (int i = 0; i < N; ++i)
            {
                const auto k = static_cast<std::size_t>(i);
                if (order == 0)
                {
                    CHECK(rig.sendL[k] == src[k]);           // the send carries the source
                    CHECK(fx.seenL[k] == pattern[k]);        // the inserts process the return
                    CHECK(l[k] == pattern[k] * 0.5f);
                }
                else
                {
                    CHECK(fx.seenL[k] == src[k]);            // the inserts process the source
                    CHECK(rig.sendL[k] == src[k] * 0.5f);    // the send carries their output
                    CHECK(l[k] == pattern[k]);               // the return replaces the path before pan
                }
            }
        }
    }
}

TEST_CASE("afs.points: an ACTIVE track route claims its silent track; the master route claims none", "[afs][route]")
{
    SlotRig rig;
    rig.set(AfsField::InsertTrack, 2);
    rig.set(AfsField::Active, 1);
    rig.settle();
    engine::stages::BlockBuffers bb {};
    bb.activeTracks = 0;
    bb.track[1].l[3] = 9.0f; // stale content of a silent track
    CHECK(rig.slot.claimTrack(bb) == 2u);
    CHECK(bb.track[1].l[3] == 0.0f);
    bb.activeTracks = 2u;
    CHECK(rig.slot.claimTrack(bb) == 0u);
    rig.set(AfsField::InsertTrack, 0);
    rig.settle();
    bb.activeTracks = 0;
    CHECK(rig.slot.claimTrack(bb) == 0u);
}

TEST_CASE("afs.external: without the FilterSend port and return inputs the model runs (our choice, the desktop)",
          "[afs][external]")
{
    SlotRig rig(false);
    rig.set(AfsField::Active, 1);
    rig.set(AfsField::Processor, 1);
    rig.settle();
    CHECK_FALSE(rig.slot.externalAvailable());
    CHECK(rig.slot.processorInUse() == Processor::Model);
    CHECK(rig.slot.latencySamples() == engine::fx::dsp::afs::kLatency);
    std::array<float, N> l {}, r {};
    rig.master(l.data(), r.data());
    CHECK(rig.sendL[0] == 0.0f);
    // A ping without a loop fails at once.
    const auto before = rig.slot.pingCount();
    rig.set(AfsField::Ping, 1);
    CHECK(rig.slot.pingCount() == before + 1);
    CHECK(rig.slot.measuredLatency() == -1);

    // The device appears: the slot switches to the loop with the dip.
    rig.loopPorts = true;
    rig.master(l.data(), r.data());
    CHECK(rig.slot.dipping());
    for (int b = 0; b < 5; ++b)
        rig.master(l.data(), r.data());
    CHECK(rig.slot.processorInUse() == Processor::External);
}

TEST_CASE("afs.ping: the external loop measures its latency on a loopback exactly (AFS-005)", "[afs][external][ping]")
{
    for (int delay : {64, 100, 777, 2000})
    {
        SlotRig rig(true);
        externalOnMaster(rig);
        std::deque<float> line(static_cast<std::size_t>(delay), 0.0f); // the analog loop: send → return after `delay`
        const auto before = rig.slot.pingCount();
        rig.set(AfsField::Ping, 1);
        std::array<float, N> ret {};
        for (int b = 0; b < 200 && rig.slot.pingCount() == before; ++b)
        {
            for (int i = 0; i < N; ++i) // this period's return = the send `delay` frames ago
            {
                ret[static_cast<std::size_t>(i)] = line.front();
                line.pop_front();
                line.push_back(0.0f); // replaced below by this block's send
            }
            std::array<float, N> l, r;
            l.fill(0.3f); // program material is muted on the send during the ping
            r.fill(0.3f);
            rig.master(l.data(), r.data(), ret.data(), ret.data());
            for (int i = 0; i < N; ++i)
                line[line.size() - N + static_cast<std::size_t>(i)] = rig.sendL[static_cast<std::size_t>(i)];
        }
        INFO("delay " << delay);
        REQUIRE(rig.slot.pingCount() == before + 1);
        CHECK(rig.slot.measuredLatency() == delay + N); // + the return adapter's block
        CHECK(rig.slot.latencySamples() == delay + N);
    }
}

TEST_CASE("afs.over: with ACTIVE off the drive still reaches the main output, and OVER still watches it", "[afs][over]")
{
    SlotRig rig;
    rig.set(AfsField::Drive, 1023);
    rig.settle(); // the drive settled at ×4
    REQUIRE_FALSE(rig.slot.active());
    std::array<float, N> l {}, r {};
    l[3] = 0.5f; // +6 dBFS after ×4: over the +5.1 dBFS threshold
    rig.master(l.data(), r.data());
    CHECK(l[3] == 2.0f);
    CHECK(rig.slot.over());
    int lit = 0;
    for (int b = 0; b < 400; ++b)
    {
        std::array<float, N> z {}, zr {};
        rig.master(z.data(), zr.data());
        lit += rig.slot.over() ? 1 : 0;
    }
    CHECK(lit * N >= 22050 - N); // held 0.5 s, then dark
    CHECK(lit * N <= 22050 + N);
    CHECK_FALSE(rig.slot.over());
}
