// Offline soak (level 1 of two; NFR-014, NFR-020, NFR-021): the real engine with the app's set-up (every ParamSpec
// bound, the FX bank's index table) and the app's project forwarding, driven faster than real time in 256-frame device
// periods on a thread marked real-time (APP_RT_CHECKS aborts on an allocation, a lock or I/O there). A seeded scenario
// does what a user does, through the bridge as the app does:
//   - every ~50 ms a random parameter of the registry (any track / scope, any value of its range: FX types and their
//     parameters, envelopes, LFOs, mixer, routing, pitch, slices …; the sampling / render / arrangement controls,
//     which need their own objects, excluded) — the walk a user makes on the knobs, far denser
//   - pad hits on all 16 tracks, scene / pattern queueing every ~1 s, a tempo change every ~10 s, PLAY / STOP / PAUSE
//     / CONTINUE every ~30 s
// Checks: every output sample of every period finite and below +40 dBFS (no NaN / Inf / runaway feedback); at the end,
// every randomised parameter back to its default and a HARD STOP: the output decays below -70 dBFS within 6 s (no
// stuck voice, no self-oscillating effect left behind); the engine rendered every block.
// Length: SOAK_MINUTES of audio (default 3: the per-merge run, a few seconds on the desktop); the hidden
// tests/unit/project/soak ([.soak]) is the 24 h variant of the project side. Same seed (SOAK_SEED, default 16) = same run.
#include "tests/unit/render/offline/RenderTestSupport.h"

#include "core/params/ParamRegistry.h"
#include "core/util/Realtime.h"
#include "engine/Engine.h"
#include "services/render/QueuedBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <string_view>

namespace rnd = services::render;

namespace
{
struct Rng
{
    std::uint32_t x;
    std::uint32_t next()
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
    float unit() { return static_cast<float>(next() >> 8) / 16777216.0f; }
};

bool excluded(std::string_view id)
{
    for (const std::string_view p : {"sampling.", "live.", "render.", "arrangement.", "transport.", "midi.", "input.",
                                     "audio.", "settings.", "prefs.", "ui.", "project.", "library."})
        if (id.substr(0, p.size()) == p)
            return true;
    return false;
}

core::ParamId idOf(std::string_view name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }
} // namespace

TEST_CASE("soak: seeded offline engine soak — random parameters, hits, scenes, transport; finite output, no stuck voice",
          "[reliability][soak]")
{
    const char* m = std::getenv("SOAK_MINUTES");
    const char* sd = std::getenv("SOAK_SEED");
    const double minutes = m != nullptr && std::atof(m) > 0 ? std::atof(m) : 3.0;
    Rng r {sd != nullptr && std::atoi(sd) != 0 ? static_cast<std::uint32_t>(std::atoi(sd)) : 16u};

    auto f = rendertest::makeFixture(true);
    // the fixture's three generated samples (kick, hat, ring) on all 16 tracks of scenes 1 and 2
    for (int sc = 0; sc < 2; ++sc)
        for (int t = 0; t < 16; ++t)
        {
            auto* b = f.buffers[static_cast<std::size_t>(t % 3)].get();
            auto& track = f.project->scenes[static_cast<std::size_t>(sc)].tracks[static_cast<std::size_t>(t)];
            track.playback.sample.id = t % 3 == 0 ? "kick" : (t % 3 == 1 ? "hat" : "ring");
            track.playback.length = b->frames;
            f.samples.set(sc, t, b);
        }
    auto engine = engine::createEngine(rnd::builtinEngineSetup());
    constexpr int kPeriod = 256;
    engine->prepare(f.rate, kPeriod);
    rnd::QueuedBridge q(engine->bridge(), 4096);
    q.post(engine::ParamChange {idOf("render.offline"), 0, 1.0f, 0}); // no test tone: the output is the project's
    app::feed::forwardAll(*f.project, q);
    app::feed::postPlan(*f.project, q, nullptr);
    app::feed::postSlices(*f.project, q, f.samples);
    for (int sc = 0; sc < 2; ++sc)
        for (int t = 0; t < 16; ++t)
            if (auto* b = f.samples.at(sc, t))
            {
                core::audio::retain(b);
                REQUIRE(q.post(engine::ObjectSwap {{engine::SwapSlot::TrackSample, static_cast<std::uint8_t>(sc),
                                                    static_cast<std::uint8_t>(t)}, b}));
            }

    // The parameters the walk may touch: every registry spec outside the excluded groups.
    std::vector<const core::ParamSpec*> specs;
    for (const auto& spec : core::builtinParams().all())
        if (! excluded(spec.id) && spec.maxValue > spec.minValue)
            specs.push_back(&spec);
    REQUIRE(specs.size() > 50);
    std::vector<std::pair<const core::ParamSpec*, std::uint16_t>> touched;

    std::vector<float> l(kPeriod), rr(kPeriod);
    float* const out[2] = {l.data(), rr.data()};
    const engine::AudioIO io {out, 1, nullptr, 0, kPeriod, 0};
    const auto block = [&]
    {
        q.flush(); // the message thread's side (the app's paced QueuedBridge), outside the real-time scope
        core::rt::ScopedRealtimeThread realtime;
        engine->process(io);
    };
    for (int i = 0; i < 64; ++i)
        block();
    q.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});

    const auto periods = static_cast<std::int64_t>(minutes * 60.0 * f.rate / kPeriod);
    const auto every = [&](double seconds) { return std::max<std::int64_t>(1, static_cast<std::int64_t>(seconds * f.rate / kPeriod)); };
    const auto sceneQueue = idOf("scene.queue");
    std::int64_t bad = 0, loud = 0, posts = 0, refused = 0, sounding = 0;
    float peak = 0;
    const float kLoud = 100.0f; // +40 dBFS
    for (std::int64_t p = 0; p < periods; ++p)
    {
        if (p % every(0.05) == 0)
        {
            const auto* spec = specs[r.below(static_cast<std::uint32_t>(specs.size()))];
            const auto scope = static_cast<std::uint16_t>(r.below(16));
            float v = spec->minValue + r.unit() * (spec->maxValue - spec->minValue);
            if (r.below(8) == 0)
                v = r.below(2) ? spec->minValue : spec->maxValue; // the ends of the range
            ++posts;
            if (! q.post(engine::ParamChange {idOf(spec->id), scope, v, static_cast<std::uint16_t>(r.below(128))}))
                ++refused;
            touched.emplace_back(spec, scope);
        }
        if (p % 3 == 0)
            engine->triggers().push({static_cast<std::uint8_t>(r.below(16)), static_cast<std::uint8_t>(40 + r.below(88)),
                                     60, -1});
        if (p % every(1.0) == 0 && sceneQueue.valid())
            q.post(engine::ParamChange {sceneQueue, 0xFFFF, static_cast<float>(r.below(2) * 16 + r.below(2)), 0});
        if (p % every(10.0) == 0)
            q.post(engine::TransportCmd {engine::TransportCmd::SetTempo, 60.0 + r.below(140)});
        if (p % every(30.0) == every(15.0))
        {
            static constexpr engine::TransportCmd::Kind kinds[] = {engine::TransportCmd::Stop,
                                                                   engine::TransportCmd::Pause,
                                                                   engine::TransportCmd::Continue};
            q.post(engine::TransportCmd {kinds[r.below(3)], 0.0});
            q.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
        }
        block();
        float periodPeak = 0;
        for (int i = 0; i < kPeriod; ++i)
        {
            const float a = l[static_cast<std::size_t>(i)], b = rr[static_cast<std::size_t>(i)];
            if (! std::isfinite(a) || ! std::isfinite(b))
                ++bad;
            else
            {
                periodPeak = std::max(periodPeak, std::max(std::fabs(a), std::fabs(b)));
                if (std::fabs(a) > kLoud || std::fabs(b) > kLoud)
                    ++loud;
            }
        }
        peak = std::max(peak, periodPeak);
        sounding += periodPeak > 1.0e-4f ? 1 : 0;
        if (p % 64 == 0)
            engine->bridge().retirePending();
    }

    // The end: every touched parameter back to its default, a hard stop, then the output must decay.
    for (const auto& [spec, scope] : touched)
        q.post(engine::ParamChange {idOf(spec->id), scope, spec->defaultValue, 0});
    q.post(engine::TransportCmd {engine::TransportCmd::HardStop, 0.0});
    for (int i = 0; i < every(1.0); ++i)
        block();
    float tail = 0;
    for (int i = 0, n = static_cast<int>(every(6.0)); i < n; ++i)
    {
        block();
        if (i >= n - every(1.0))
            for (int k = 0; k < kPeriod; ++k)
                tail = std::max(tail, std::max(std::fabs(l[static_cast<std::size_t>(k)]), std::fabs(rr[static_cast<std::size_t>(k)])));
    }
    engine->bridge().retirePending();
    const double tailDb = tail > 0 ? 20.0 * std::log10(static_cast<double>(tail)) : -200.0;
    std::printf("soak offline: %.1f min of audio, %lld periods, %lld parameter posts (%lld not engine parameters), "
                "sounding %.0f %%, peak %.1f dBFS, non-finite samples %lld, > +40 dBFS %lld, tail %.1f dBFS, engine blocks %llu\n",
                minutes, static_cast<long long>(periods), static_cast<long long>(posts), static_cast<long long>(refused),
                100.0 * static_cast<double>(sounding) / static_cast<double>(std::max<std::int64_t>(1, periods)),
                peak > 0 ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0, static_cast<long long>(bad), static_cast<long long>(loud),
                tailDb, static_cast<unsigned long long>(engine->blocksRendered()));
    CHECK(sounding > periods / 4); // the set plays most of the time (hits, the pattern, tails)
    CHECK(bad == 0);
    CHECK(loud == 0);
    CHECK(tailDb <= -70.0);
    CHECK(engine->blocksRendered() >= static_cast<std::uint64_t>(periods) * (kPeriod / 64));
    engine.reset(); // before the fixture's buffers
}
