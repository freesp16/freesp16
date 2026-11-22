// RetriggerScheduler on its own (spec 02-features/sequencer.md SEQ-030, OQ-09): repeat k of a step at
// frame t0 fires at t0 + round(k · fraction · stepSamples), exactly, across block boundaries, for every count 1–16,
// all six speeds and the step lengths of 60 / 120 / 300 BPM at 44.1 / 48 kHz; a new step cancels; locks are copied.
#include "engine/sequencer/perf/Retrigger.h"

#include "core/model/enums/ValueLists.h"
#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using engine::sequencer::perf::RetriggerScheduler;
using engine::sequencer::perf::RetrigSpec;
using engine::trigger::TriggerEvent;
using engine::trigger::TriggerKind;
using engine::trigger::TriggerSource;
namespace perf = engine::sequencer::perf;

namespace
{
struct Recorder
{
    std::vector<TriggerEvent> events;
    std::vector<long long> abs; // absolute frame
    long long blockStart = 0;
    Recorder()
    {
        events.reserve(64); // no allocation inside the real-time scope
        abs.reserve(64);
    }
    bool push(const TriggerEvent& e)
    {
        events.push_back(e);
        abs.push_back(blockStart + e.frame);
        return true;
    }
};
struct Counter
{
    int n = 0;
    bool push(const TriggerEvent&) noexcept
    {
        ++n;
        return true;
    }
};

// Runs blocks until nothing is pending; the recorder gets absolute frames.
void drain(RetriggerScheduler& s, Recorder& r, int maxBlocks = 100000)
{
    for (int b = 0; b < maxBlocks && s.pendingMask() != 0; ++b)
    {
        r.blockStart = s.clock();
        {
            core::rt::ScopedRealtimeThread realtime;
            s.emitBlock(r);
        }
    }
}
} // namespace

TEST_CASE("perf.retrig: the speed table equals the model's RETRIGGER SPD fractions", "[perf][retrig]")
{
    using core::model::RetriggerSpeed;
    for (int i = 0; i < perf::kRetrigSpeeds; ++i)
        CHECK(perf::retrigFraction(i) == core::model::retriggerStepFraction(static_cast<RetriggerSpeed>(i)));
    CHECK(perf::retrigFraction(perf::kDefaultRetrigSpeed) == 0.25); // SEQ-030: default 1/4 step (M p.40)
    CHECK(perf::stepSamplesAt(44100.0, 120.0) == 5512.5);
}

TEST_CASE("perf.retrig: every ratchet onset lands on t0 + round(k * speed * step), counts 1-16, all speeds, 60/120/300 BPM",
          "[perf][retrig]")
{
    for (const double rate : {44100.0, 48000.0})
        for (const double bpm : {60.0, 120.0, 300.0})
            for (int speed = 0; speed < perf::kRetrigSpeeds; ++speed)
                for (int count = 1; count <= 16; ++count)
                    for (const int offset : {0, 17, 63})
                    {
                        INFO("rate " << rate << " bpm " << bpm << " speed " << speed << " count " << count << " offset "
                                     << offset);
                        RetriggerScheduler s;
                        for (int b = 0; b < 3; ++b) // the step lands in block 3, not at clock 0
                            s.endBlock();
                        const long long t0 = s.clock() + offset;
                        const double step = perf::stepSamplesAt(rate, bpm);
                        Recorder r;
                        s.onStep(5, offset, 99, RetrigSpec {static_cast<std::uint8_t>(count), static_cast<std::uint8_t>(speed)},
                                 step);
                        CHECK(s.remaining(5) == (count >= 2 ? count - 1 : 0));
                        drain(s, r);
                        REQUIRE(r.events.size() == static_cast<std::size_t>(count - 1));
                        // Exact: 2 · step is whole at these rates and tempos; k · step / divisor rounded half up.
                        const auto step2 = static_cast<long long>(2.0 * step);
                        REQUIRE(static_cast<double>(step2) == 2.0 * step);
                        const long long d = perf::retrigDivisor(speed);
                        for (int k = 1; k < count; ++k)
                        {
                            const auto& e = r.events[static_cast<std::size_t>(k - 1)];
                            CHECK(r.abs[static_cast<std::size_t>(k - 1)] == t0 + (k * step2 + d) / (2 * d));
                            CHECK(e.track == 5);
                            CHECK(e.velocity == 99);
                            CHECK(e.kind == TriggerKind::Full);
                            CHECK(e.source == TriggerSource::Retrigger);
                            CHECK(e.locks == nullptr);
                        }
                    }
}

TEST_CASE("perf.retrig: a new step of the track cancels its pending repeats; other tracks keep theirs", "[perf][retrig]")
{
    RetriggerScheduler s;
    const double step = perf::stepSamplesAt(44100.0, 120.0);
    s.onStep(0, 0, 100, {16, 5}, step); // 15 repeats, one step apart
    s.onStep(1, 0, 100, {4, 5}, step);
    Recorder r;
    for (int b = 0; b < 100; ++b) // ≈ 1.16 steps: one repeat of each
    {
        r.blockStart = s.clock();
        s.emitBlock(r);
    }
    CHECK(r.events.size() == 2u);
    s.onStep(0, 10, 100, {1, 5}, step); // count 1: cancels, schedules nothing
    CHECK_FALSE(s.pending(0));
    CHECK(s.pending(1));
    s.onStep(1, 20, 80, {3, 4}, step); // a new ratchet replaces the old one
    CHECK(s.remaining(1) == 2);
    const long long t0 = s.clock() + 20;
    r = Recorder {};
    drain(s, r);
    REQUIRE(r.events.size() == 2u);
    CHECK(r.abs[0] == t0 + static_cast<long long>(std::floor(0.5 * step + 0.5)));
    CHECK(r.abs[1] == t0 + static_cast<long long>(std::floor(1.0 * step + 0.5)));
    CHECK(r.events[0].velocity == 80);
    s.onStep(2, 0, 100, {8, 0}, step);
    s.cancelAll(); // STOP
    CHECK(s.pendingMask() == 0);
}

TEST_CASE("perf.retrig: HALF ratchets stay HALF, PARAM steps never ratchet; the note offset is kept", "[perf][retrig]")
{
    RetriggerScheduler s;
    TriggerEvent e {};
    e.track = 3;
    e.velocity = 64;
    e.semitones = -5;
    e.kind = TriggerKind::Half;
    e.source = TriggerSource::Sequencer;
    e.frame = 7;
    s.onStep(e, {3, 2}, 1000.0);
    Recorder r;
    drain(s, r);
    REQUIRE(r.events.size() == 2u);
    CHECK(r.events[0].kind == TriggerKind::Half);
    CHECK(r.events[0].semitones == -5);
    CHECK(r.abs[0] == 7 + 250);
    CHECK(r.abs[1] == 7 + 500);
    e.kind = TriggerKind::LockOnly;
    s.onStep(e, {8, 2}, 1000.0);
    CHECK_FALSE(s.pending(3));
}

TEST_CASE("perf.retrig: the step's locks are copied, so repeats outlive the pattern object", "[perf][retrig]")
{
    RetriggerScheduler s;
    std::vector<float> values(35, 0.0f);
    std::vector<std::uint8_t> change(35, 0);
    values[4] = 12.0f; // PITCH
    values[16] = 4.0f; // RETRIGGER CNT
    change[4] = 1;     // Hold
    const engine::trigger::LockSet set {(1ull << 4) | (1ull << 16), values.data(), change.data()};
    TriggerEvent e {};
    e.track = 9;
    e.velocity = 127;
    e.kind = TriggerKind::Full;
    e.source = TriggerSource::Sequencer;
    e.frame = 0;
    e.locks = &set;
    s.onStep(e, {4, 0}, 2000.0);
    values.assign(35, -1.0f); // the pattern object is gone (retired) before the repeats fire
    change.assign(35, 9);
    Recorder r;
    drain(s, r);
    REQUIRE(r.events.size() == 3u);
    for (const auto& ev : r.events)
    {
        REQUIRE(ev.locks != nullptr);
        CHECK(ev.locks->mask == ((1ull << 4) | (1ull << 16)));
        float v = 0.0f;
        CHECK(ev.locks->get(4, v));
        CHECK(v == 12.0f);
        CHECK(ev.locks->change[4] == 1);
        CHECK_FALSE(ev.locks->get(5, v));
    }
}

TEST_CASE("perf.retrig: emitUntil stops before a frame; 16 tracks x 15 repeats in one block, no allocation",
          "[perf][retrig]")
{
    RetriggerScheduler s;
    s.onStep(0, 0, 100, {16, 0}, 16.0); // interval 2 frames
    Recorder r;
    CHECK(s.emitUntil(0, 9, r) == 4); // frames 2, 4, 6, 8
    CHECK(s.nextFrame(0) == 10);
    for (int t = 0; t < 16; ++t)
        s.onStep(t, 1, 100, {16, 0}, 32.0); // interval 4: 15 repeats at 5 … 61
    Counter c;
    {
        core::rt::ScopedRealtimeThread realtime;
        CHECK(s.emitBlock(c) == 16 * 15);
    }
    CHECK(s.pendingMask() == 0);
    // Sub-frame intervals are clamped to one frame (our choice).
    s.onStep(0, 0, 100, {16, 0}, 2.0);
    CHECK(s.emitBlock(c) == 15);
}
