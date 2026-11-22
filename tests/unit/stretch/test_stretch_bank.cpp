// StretcherBank (engine/voice/stretch/StretcherBank.h): the per-voice set of the selected backend, the RT-safe swap,
// the priming budget, the sample tempos, the governor wiring; and the engine's bank (Engine::stretchBank()).
#include "core/util/Realtime.h"
#include "engine/Engine.h"
#include "engine/voice/stretch/StretcherBank.h"
#include "tests/unit/stretch/StretchHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using engine::voice::stretch::StretcherBank;

TEST_CASE("stretch bank: prepare builds the default backend", "[stretch][bank]")
{
    StretcherBank bank;
    bank.prepare(44100.0, 64);
    CHECK(bank.installedName() == StretcherBank::kDefaultBackend);
    CHECK(bank.selected() == std::string(StretcherBank::kDefaultBackend));
    for (int t = 0; t < StretcherBank::kLanes; ++t)
        CHECK(bank.lane(t) != nullptr);
    CHECK(bank.lane(0) != bank.lane(1));
    CHECK(bank.scratchFrames() >= 64);
    CHECK(bank.scratch(0) != nullptr);
    CHECK(bank.scratch(1) != nullptr);
}

TEST_CASE("stretch bank: select queues a set, the audio thread installs it, collect frees the old one", "[stretch][bank]")
{
    StretcherBank bank;
    bank.prepare(44100.0, 64);
    const auto gen = bank.generation();
    std::vector<std::string> lines;
    REQUIRE(bank.select("pvoc", [&](const std::string& l) { lines.push_back(l); }));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("STRETCH backend=pvoc") != std::string::npos);
    CHECK(bank.installedName() == StretcherBank::kDefaultBackend); // not before the next block
    {
        core::rt::ScopedRealtimeThread realtime; // the install allocates and frees nothing
        bank.beginBlock({});
    }
    CHECK(bank.installedName() == "pvoc");
    CHECK(bank.generation() != gen);
    bank.collect();
    // A second select while the first is still queued replaces it.
    REQUIRE(bank.select("wsola"));
    REQUIRE(bank.select("none"));
    {
        core::rt::ScopedRealtimeThread realtime;
        bank.beginBlock({});
    }
    CHECK(bank.installedName() == "none");
    CHECK(bank.lane(3) == nullptr); // "none": MASTER TEMPO plays as RESAMPLE
    bank.collect();
    // Unknown names change nothing.
    lines.clear();
    CHECK_FALSE(bank.select("no-such-backend", [&](const std::string& l) { lines.push_back(l); }));
    CHECK(bank.selected() == "none");
    CHECK(lines.size() == 1);
}

TEST_CASE("stretch bank: two primes per block, sample tempos per scene and track", "[stretch][bank]")
{
    StretcherBank bank;
    bank.prepare(44100.0, 64);
    bank.beginBlock({});
    CHECK(bank.tryPrime());
    CHECK(bank.tryPrime());
    CHECK_FALSE(bank.tryPrime());
    bank.beginBlock({});
    CHECK(bank.tryPrime());
    bank.setSampleTempo(3, 7, 98.5f);
    CHECK(bank.sampleTempo(3, 7) == 98.5f);
    CHECK(bank.sampleTempo(7, 3) == 0.0f);
    bank.setSampleTempo(3, 7, -1.0f);
    CHECK(bank.sampleTempo(3, 7) == 0.0f);
}

TEST_CASE("stretch bank: the governor sees last block's players", "[stretch][bank]")
{
    StretcherBank bank;
    bank.prepare(44100.0, 64);
    bank.beginBlock({1, 33, 0});
    CHECK(bank.decision().tier == 1);
    CHECK(bank.tierNow() == 1);
    for (int i = 0; i < 6; ++i)
        bank.countPlayer();
    bank.beginBlock({1, 33, 0});
    CHECK(bank.decision().score == 8 + 33 + 12);
    CHECK(bank.holdNew());
    CHECK(bank.holding());
    bank.beginBlock({});
    CHECK_FALSE(bank.holdNew());
    CHECK(bank.tierNow() == 0);
}

TEST_CASE("stretch bank: the engine prepares one bank and installs a selected backend", "[stretch][bank]")
{
    engine::Engine e({}, engine::EngineOptions {.testTone = false});
    e.prepare(44100.0, 256);
    CHECK(e.stretchBank().installedName() == StretcherBank::kDefaultBackend);
    REQUIRE(e.stretchBank().select("pvoc"));
    std::vector<float> l(256), r(256);
    float* out[2] = {l.data(), r.data()};
    engine::OfflineDriver{}.run(e, 256, out);
    CHECK(e.stretchBank().installedName() == "pvoc");
    e.stretchBank().collect();
}

TEST_CASE("stretch bank: grain staggering moves the work, not the output", "[stretch][bank]")
{
    for (const char* backend : {"pvoc", "wsola"})
    {
        INFO("backend " << backend);
        StretcherBank bank;
        bank.select(backend);
        bank.prepare(44100.0, 64);
        const auto in = stretchtest::sine(100000, 523.25);
        auto* a = bank.lane(0);
        auto* b = bank.lane(5);
        for (auto* s : {a, b})
        {
            s->setRatios(1.3, 0.9);
            s->reset();
        }
        const auto oa = stretchtest::run(*a, in, in, 20000);
        const auto ob = stretchtest::run(*b, in, in, 20000);
        CHECK(oa.l == ob.l);
        CHECK(oa.r == ob.r);
        CHECK(oa.consumedAt != ob.consumedAt); // lane 5 pulls its grains' input earlier
    }
}
