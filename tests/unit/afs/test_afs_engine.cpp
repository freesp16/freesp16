// The analog-filter slot inside engine::Engine: afs.* bound and accepted through the bridge; MAIN through
// the model on the Master route and on a track route (pre / post FX), click-free route and ACTIVE switching on MAIN;
// the desktop fallback (no FilterSend port: the model runs) and the external loop on a loopback device — FilterSend
// out, return on input channels 2/3 — with its latency measured to the sample through the real block adapter.
#include "tests/unit/afs/AfsTestUtil.h"
#include "tests/unit/engine/EngineTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <deque>

using namespace afstest;

namespace
{
void post(engine::Engine& e, std::string_view id, float v)
{
    REQUIRE(e.bridge().post(engine::ParamChange {paramId(id), 0, v, 0}));
}

// MAIN of `frames` frames (the TEMPORARY 440 Hz −12 dBFS test tone on track 1 is the source).
enginetest::Stereo renderMain(engine::Engine& e, int frames, int period = 64) { return enginetest::render(e, frames, period); }

double toneRms(const enginetest::Stereo& s) { return rms(s.l, s.l.size() / 2, s.l.size()); }
} // namespace

TEST_CASE("afs.engine: every afs.* id is registered, bound and accepted", "[afs][engine]")
{
    auto e = enginetest::make(false);
    for (int f = 0; f < engine::afs::AnalogFilterSlot::kParams; ++f)
    {
        const auto id = engine::afs::kAfsParamIds[f];
        INFO(id);
        REQUIRE(paramId(id).valid());
        CHECK(e->afs().paramId(f) == paramId(id));
        CHECK(e->bridge().post(engine::ParamChange {paramId(id), 0, 0.0f, 0}));
    }
    CHECK(paramId("afs.latency").valid()); // the app's per-device preference (not an engine id)
}

TEST_CASE("afs.engine: ACTIVE off is bit-transparent on MAIN; the Master route filters MAIN", "[afs][engine]")
{
    auto ref = enginetest::make(true);
    auto e = enginetest::make(true);
    post(*e, "afs.lpCutoff", 100); // ACTIVE off: no effect at all (DRIVE 0 = unity)
    const auto a = renderMain(*ref, 8192), b = renderMain(*e, 8192);
    CHECK(sameBits(a.l, b.l));
    CHECK(sameBits(a.r, b.r));
    {
        // DRIVE drives the main output with ACTIVE off too, as on the reference (F107: dsfDriveMaster): 1023 = ×4.
        auto d = enginetest::make(true);
        post(*d, "afs.drive", 1023);
        const auto c = renderMain(*d, 8192);
        CHECK(enginetest::peak(c.l, 4096) > 2.0f * enginetest::peak(a.l, 4096));
    }

    post(*e, "afs.drive", 0);
    post(*e, "afs.lpCutoff", 1023);
    post(*e, "afs.active", 1);
    renderMain(*e, 4096);
    const double open = toneRms(renderMain(*e, 8192));
    // 440 Hz −12 dBFS through the open filter: the four stage saturators compress ≈ 0.07 dB each at this level.
    CHECK(std::fabs(20.0 * std::log10(open / toneRms(renderMain(*ref, 8192)))) < 0.5);
    post(*e, "afs.lpCutoff", 150); // ≈ 75 Hz: −24 dB/oct over ≈ 2.5 octaves
    renderMain(*e, 4096);
    const double closed = toneRms(renderMain(*e, 8192));
    CHECK(20.0 * std::log10(closed / open) < -40.0);
    CHECK(e->afs().route() == Route::Master);
    CHECK(e->afs().latencySamples() == 31);
}

TEST_CASE("afs.engine: a track route filters only that track; pre and post FX", "[afs][engine]")
{
    for (int order : {0, 1})
    {
        auto e = enginetest::make(true);
        post(*e, "afs.insertTrack", 2); // TRACK 2: the tone (track 1) passes
        post(*e, "afs.insertOrder", static_cast<float>(order));
        post(*e, "afs.lpCutoff", 100);
        post(*e, "afs.active", 1);
        renderMain(*e, 4096);
        const double t2 = toneRms(renderMain(*e, 8192));
        post(*e, "afs.insertTrack", 1); // TRACK 1: the tone is filtered away
        renderMain(*e, 4096);
        const double t1 = toneRms(renderMain(*e, 8192));
        INFO("order " << order);
        CHECK(e->afs().route() == (order == 0 ? Route::TrackPreFx : Route::TrackPostFx));
        CHECK(e->afs().track() == 0);
        CHECK(t2 > 0.05);
        CHECK(20.0 * std::log10(t1 / t2) < -40.0);
    }
}

TEST_CASE("afs.engine: switching route, order and ACTIVE is click-free on MAIN (AFS-006)", "[afs][engine][dip]")
{
    auto e = enginetest::make(true);
    post(*e, "afs.lpCutoff", 1023);
    renderMain(*e, 4096);
    const auto steady = renderMain(*e, 4096);
    const float toneStep = enginetest::maxStep(steady.l);
    const char* const ids[] = {"afs.active", "afs.insertTrack", "afs.insertOrder", "afs.insertTrack", "afs.active",
                               "afs.processor", "afs.active"};
    const float values[] = {1, 1, 1, 0, 0, 1, 1};
    for (std::size_t i = 0; i < std::size(ids); ++i)
    {
        post(*e, ids[i], values[i]);
        const auto s = renderMain(*e, 2048);
        INFO(ids[i] << " = " << values[i]);
        CHECK(enginetest::maxStep(s.l) <= 1.25f * toneStep); // a 256-sample dip, never a jump
        CHECK(enginetest::maxStep(s.r) <= 1.25f * toneStep);
    }
}

TEST_CASE("afs.engine: on the desktop (no FilterSend port) EXTERNAL runs the model (our choice)",
          "[afs][engine][external]")
{
    auto e = enginetest::make(true);
    post(*e, "afs.active", 1);
    post(*e, "afs.processor", 1);
    renderMain(*e, 2048);
    CHECK_FALSE(e->afs().externalAvailable());
    CHECK(e->afs().processorInUse() == engine::afs::Processor::Model);
    CHECK(toneRms(renderMain(*e, 4096)) > 0.05);
}

namespace
{
// A device with every port and four inputs whose channels 2/3 are the FilterSend pair `delay` frames later.
struct Loopback
{
    int delay, period;
    std::deque<float> lineL, lineR;
    std::vector<std::vector<float>> out;
    std::vector<float> in0, in1, in2, in3;
    Loopback(int d, int p) : delay(d), period(p), lineL(static_cast<std::size_t>(d), 0.0f), lineR(lineL)
    {
        out.assign(2 * engine::stages::kPorts, std::vector<float>(static_cast<std::size_t>(p)));
        in0.assign(static_cast<std::size_t>(p), 0.0f);
        in1 = in2 = in3 = in0;
    }
    void run(engine::Engine& e)
    {
        constexpr auto fs = static_cast<std::size_t>(engine::OutPort::FilterSend);
        for (int i = 0; i < period; ++i) // this period's return = the send `delay` frames ago (delay ≥ period)
        {
            in2[static_cast<std::size_t>(i)] = lineL.front();
            in3[static_cast<std::size_t>(i)] = lineR.front();
            lineL.pop_front();
            lineR.pop_front();
        }
        std::array<float*, 2 * engine::stages::kPorts> o {};
        for (std::size_t c = 0; c < o.size(); ++c)
            o[c] = out[c].data();
        const std::array<const float*, 4> in {in0.data(), in1.data(), in2.data(), in3.data()};
        e.process(engine::AudioIO {o.data(), engine::stages::kPorts, in.data(), 4, period, 0});
        for (int i = 0; i < period; ++i)
        {
            lineL.push_back(out[2 * fs][static_cast<std::size_t>(i)]);
            lineR.push_back(out[2 * fs + 1][static_cast<std::size_t>(i)]);
        }
    }
};
} // namespace

TEST_CASE("afs.engine: the external loop on a loopback — FilterSend out, return in, latency to the sample (AFS-005)",
          "[afs][engine][external][ping]")
{
    for (auto [delay, period] : {std::pair {64, 64}, std::pair {300, 256}, std::pair {517, 128}, std::pair {1500, 512}})
    {
        auto e = enginetest::make(true);
        Loopback dev(delay, period);
        post(*e, "afs.active", 1);
        post(*e, "afs.processor", 1);
        for (int p = 0; p < 16; ++p)
            dev.run(*e);
        INFO("delay " << delay << " period " << period);
        REQUIRE(e->afs().externalAvailable());
        REQUIRE(e->afs().processorInUse() == engine::afs::Processor::External);
        // The tone goes out on FilterSend and comes back on MAIN.
        double sendSum = 0, mainSum = 0;
        for (int p = 0; p < 8; ++p)
        {
            dev.run(*e);
            for (int i = 0; i < period; ++i)
            {
                sendSum += std::fabs(static_cast<double>(dev.out[2 * static_cast<int>(engine::OutPort::FilterSend)][static_cast<std::size_t>(i)]));
                mainSum += std::fabs(static_cast<double>(dev.out[0][static_cast<std::size_t>(i)]));
            }
        }
        CHECK(sendSum > 1.0);
        CHECK(mainSum > 1.0);

        const auto before = e->afs().pingCount();
        post(*e, "afs.ping", 1);
        for (int p = 0; p < 400 && e->afs().pingCount() == before; ++p)
            dev.run(*e);
        REQUIRE(e->afs().pingCount() == before + 1);
        CHECK(e->afs().measuredLatency() == delay + engine::Engine::inputLatencyFrames());
    }
}
