// Insert Delay and Send/Master Delay behaviour (spec 02-features/effects/{insert,send}-delay.md). The
// Reference profile is the golden (tests/golden/{delay,delayBus}, bit-exact with the research models); these check the
// Sixteen profile's fixes (FX-680/681/682, FX-780/781/782), exact integer echo positions, the parameter laws (free and
// SYNC times with their clamps, feedback, filter steps), spread / ping-pong, the parameter catalogue and texts, and the
// memory each instance and the 34-slot bank pre-allocate.
#include "tests/unit/fx/FxTestUtil.h"

#include "engine/fx/FxShell.h"
#include "engine/fx/effects/InsertDelay.h"
#include "engine/fx/effects/Reverb.h"
#include "engine/fx/effects/SendDelay.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace engine::fx;

namespace
{
constexpr int N = engine::kBlockFrames;

template <typename E> std::unique_ptr<E> make(double fs, std::initializer_list<std::pair<int, float>> knobs,
                                             std::uint32_t quirks = kQuirkProfileSixteen, bool sync = false)
{
    auto e = std::make_unique<E>();
    e->prepare(fs, N);
    e->setQuirkProfile(quirks);
    for (auto [k, v] : knobs)
        e->setParam(k, v);
    e->setOption(0, sync ? 1 : 0);
    e->reset();
    return e;
}

struct Out
{
    std::vector<float> l, r;
};
template <typename E>
Out run(E& e, const std::vector<float>& inL, const std::vector<float>& inR, double bpm = 120.0, double fs = 44100.0)
{
    Out o;
    o.l.resize(inL.size());
    o.r.resize(inR.size());
    alignas(16) float l[N], r[N];
    const ProcessContext ctx = fxtest::context(fs, bpm);
    for (std::size_t b = 0; b + N <= inL.size(); b += N)
    {
        std::memcpy(l, inL.data() + b, sizeof l);
        std::memcpy(r, inR.data() + b, sizeof r);
        e.process(l, r, ctx);
        std::memcpy(o.l.data() + b, l, sizeof l);
        std::memcpy(o.r.data() + b, r, sizeof r);
    }
    return o;
}

std::vector<float> impulse(std::size_t n, std::size_t at, float a = 1.0f)
{
    std::vector<float> v(n, 0.0f);
    if (at < n)
        v[at] = a;
    return v;
}
std::vector<float> noise(std::size_t n, unsigned seed, std::size_t len)
{
    std::vector<float> v(n, 0.0f);
    for (std::size_t i = 0; i < n && i < len; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        v[i] = 0.5f * static_cast<float>(static_cast<int>(seed >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
    }
    return v;
}
int firstNonZero(const std::vector<float>& v, std::size_t from = 0, float above = 0.0f)
{
    for (std::size_t i = from; i < v.size(); ++i)
        if (std::fabs(v[i]) > above)
            return static_cast<int>(i);
    return -1;
}
bool silentFrom(const Out& o, std::size_t from)
{
    return firstNonZero(o.l, from) < 0 && firstNonZero(o.r, from) < 0;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- laws

TEST_CASE("fx.delay: time, sync, feedback and hi-cut laws (FX-620…623)", "[fx][delay]")
{
    CHECK(InsertDelay::freeMs(0.0f) == 10.0f);
    CHECK(InsertDelay::freeMs(49.0f / 99.0f) == 500.0f);
    CHECK(InsertDelay::freeMs(1.0f) == 1000.0f);
    CHECK(InsertDelay::syncMs(0.0f, 120.0f) == 62.5f);                       // 1/8 beat
    CHECK(InsertDelay::syncMs(0.4f, 120.0f) == 250.0f);                      // 1/2 beat
    CHECK(InsertDelay::syncMs(1.0f, 120.0f) == 1000.0f);                     // 2 beats
    CHECK(InsertDelay::syncMs(1.0f, 60.0f) == 1000.0f);                      // 2 beats at 60 BPM clamp (FX-623)
    CHECK(InsertDelay::syncMs(0.0f, 999.0f) == 10.0f);                       // 1/8 at 999 BPM: 7.5 ms → 10
    CHECK(InsertDelay::syncMs(0.6f, 87.5f) == Catch::Approx(514.2857f));     // 3/4 beat
    CHECK(InsertDelay::feedbackGain(0.7f) == Catch::Approx(0.7f));
    CHECK(InsertDelay::feedbackGain(0.704f) == Catch::Approx(0.7f));        // 0.01 steps
    CHECK(InsertDelay::feedbackGain(1.0f) == 1.0f);
    CHECK(InsertDelay::hiCutStepOf(0.0f) == 1u);
    CHECK(InsertDelay::hiCutStepOf(1.0f) == 1000u);
    CHECK(SendDelay::freeMs(490.0f / 3990.0f) == 500.0f);
    CHECK(SendDelay::freeMs(1.0f) == 4000.0f);
    CHECK(SendDelay::syncMs(1.0f, 120.0f) == 4000.0f);                       // 8 beats at 120 BPM
    CHECK(SendDelay::syncMs(1.0f, 60.0f) == 4000.0f);                        // 8 beats below 120 BPM: clamped
    CHECK(SendDelay::syncMs(6.0f / 7.0f, 60.0f) == 4000.0f);                 // 4 beats at 60 BPM
    CHECK(SendDelay::syncMs(6.0f / 7.0f, 59.0f) == 4000.0f);                 // clamped (4067.8 ms)
    CHECK(SendDelay::syncMs(0.0f, 133.37f) == Catch::Approx(56.235f).epsilon(1e-4));
    CHECK(SendDelay::loCutStepOf(0.0f) == 1u);
    CHECK(SendDelay::loCutStepOf(1.0f) == 1000u);
    CHECK(SendDelay::loCutStepOf(0.25f) == 683u); // 1000^(682/999) = 111.7 Hz (spec table)
}

// ---------------------------------------------------------------------------------------------------- Insert Delay

TEST_CASE("fx.delay: integer echo positions exact, feedback after the hi-cut (FX-620)", "[fx][delay]")
{
    for (double fs : {44100.0, 48000.0})
        for (float v : {0.0f, 0.25f, 49.0f / 99.0f, 1.0f})
        {
            auto e = make<InsertDelay>(fs, {{0, v}, {1, 0.5f}, {2, 1.0f}});
            const int D = static_cast<int>((InsertDelay::freeMs(v) * 0.001f) * static_cast<float>(fs));
            CHECK(e->delaySamples() == D);
            const std::size_t at = 300, n = static_cast<std::size_t>(at + 3 * D + 2 * N) / N * N;
            const auto o = run(*e, impulse(n, at), impulse(n, at), 120.0, fs);
            INFO("fs " << fs << " v " << v << " D " << D);
            CHECK(firstNonZero(o.l) == static_cast<int>(at) + D);
            CHECK(firstNonZero(o.l, at + static_cast<std::size_t>(D) + 100, 1e-3f) == static_cast<int>(at) + 2 * D);
            CHECK(firstNonZero(o.r) == static_cast<int>(at) + D);
        }
}

TEST_CASE("fx.delay: Sixteen fixes vs the Reference profile (FX-680/681/682)", "[fx][delay]")
{
    const double fs = 44100.0;
    SECTION("FX-680: an impulse at t = 0 echoes at D")
    {
        auto six = make<InsertDelay>(fs, {{0, 49.0f / 99.0f}, {1, 0.7f}, {2, 1.0f}}, kQuirkProfileSixteen);
        auto ref = make<InsertDelay>(fs, {{0, 49.0f / 99.0f}, {1, 0.7f}, {2, 1.0f}}, kQuirkProfileReference);
        const std::size_t n = 64 * 400;
        const auto a = run(*six, impulse(n, 0), impulse(n, 0));
        const auto b = run(*ref, impulse(n, 0), impulse(n, 0));
        CHECK(firstNonZero(a.l) == 22050);
        CHECK(std::fabs(b.l[22050]) < std::fabs(a.l[22050])); // the reference's warm-up gate is still closed
    }
    SECTION("FX-681: no pre-reset audio after a reset with a time change")
    {
        for (auto q : {kQuirkProfileSixteen, kQuirkProfileReference})
        {
            auto e = make<InsertDelay>(fs, {{0, 0.3f}, {1, 0.8f}, {2, 1.0f}}, q);
            const std::size_t n = 64 * 800;
            (void) run(*e, noise(n, 3u, n), noise(n, 4u, n));
            e->setParam(0, 0.95f);
            e->reset();
            const auto o = run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f));
            INFO("profile " << q);
            CHECK(silentFrom(o, 0) == (q == kQuirkProfileSixteen));
        }
    }
    SECTION("FX-682: a first time equal to the constructor's applies")
    {
        auto six = make<InsertDelay>(fs, {{0, 0.75f}}, kQuirkProfileSixteen);
        auto ref = make<InsertDelay>(fs, {{0, 0.75f}}, kQuirkProfileReference);
        CHECK(six->delaySamples() == static_cast<int>((InsertDelay::freeMs(0.75f) * 0.001f) * 44100.0f));
        CHECK(ref->delaySamples() == 2205); // the module's 50 ms
    }
}

TEST_CASE("fx.delay: SYNC follows the tempo (FX-623), the time change gates (FX-622)", "[fx][delay]")
{
    auto e = make<InsertDelay>(44100.0, {{0, 0.4f}, {1, 0.0f}}, kQuirkProfileSixteen, true);
    const std::size_t n = 64 * 400;
    (void) run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f), 120.0);
    CHECK(e->delaySamples() == 11025); // 1/2 beat at 120 BPM
    (void) run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f), 60.0);
    CHECK(e->delaySamples() == 22050);
    (void) run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f), 999.0);
    CHECK(e->delayMs() == Catch::Approx(30.03003f));
    e->setOption(InsertDelay::kOptSync, 0);
    (void) run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f), 999.0);
    CHECK(e->delayMs() == InsertDelay::freeMs(0.4f));
}

TEST_CASE("fx.delay: kClearState returns to the constructor state (FX-036)", "[fx][delay]")
{
    auto a = make<InsertDelay>(44100.0, {{0, 0.1f}, {1, 0.9f}, {2, 0.3f}, {5, 1.0f}});
    const std::size_t n = 64 * 300;
    (void) run(*a, noise(n, 9u, n), noise(n, 10u, n));
    a->setOption(kClearState, 1);
    auto b = std::make_unique<InsertDelay>();
    b->prepare(44100.0, N);
    for (auto* e : {a.get(), b.get()})
    {
        e->setParam(0, 0.2f);
        e->setParam(1, 0.5f);
        e->setParam(2, 0.6f);
        e->reset();
    }
    CHECK(a->hiCutStep() == b->hiCutStep());
    const auto x = run(*a, noise(n, 1u, n), noise(n, 2u, n)), y = run(*b, noise(n, 1u, n), noise(n, 2u, n));
    CHECK(std::memcmp(x.l.data(), y.l.data(), x.l.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(x.r.data(), y.r.data(), x.r.size() * sizeof(float)) == 0);
}

// ---------------------------------------------------------------------------------------------------- Send Delay

TEST_CASE("fx.delayBus: spread 0 straight, 1 ping-pong (FX-721)", "[fx][delayBus]")
{
    const double fs = 44100.0;
    const float v300 = 290.0f / 3990.0f; // 300 ms = 13230 samples
    {
        auto e = make<SendDelay>(fs, {{0, v300}, {1, 0.8f}, {4, 0.0f}});
        const std::size_t n = 64 * 700;
        const auto o = run(*e, impulse(n, 100), std::vector<float>(n, 0.0f));
        CHECK(firstNonZero(o.l) == 100 + 13230);
        CHECK(firstNonZero(o.r) < 0); // L-only stays left
    }
    {
        auto e = make<SendDelay>(fs, {{0, v300}, {1, 0.8f}, {4, 1.0f}});
        const std::size_t n = 64 * 700;
        const auto o = run(*e, impulse(n, 100), std::vector<float>(n, 0.0f));
        CHECK(firstNonZero(o.r) == 100 + 13230);     // first on the right after D
        CHECK(firstNonZero(o.l) == 100 + 2 * 13230); // then on the left after 2D
    }
}

TEST_CASE("fx.delayBus: feedback 1 sustains (FX-783), the filters stay in the loop (FX-722/723)", "[fx][delayBus]")
{
    const double fs = 44100.0;
    auto e = make<SendDelay>(fs, {{0, 0.0f}, {1, 1.0f}, {2, 1.0f}, {3, 0.0f}}); // 10 ms, unity loop gain
    const std::size_t n = 64 * 200;
    std::vector<float> in(n, 0.0f);
    for (std::size_t i = 100; i < 300; ++i) // a 200-sample 441 Hz burst
        in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 441.0f * static_cast<float>(i) / 44100.0f);
    const auto o = run(*e, in, in);
    auto peak = [&](std::size_t from, std::size_t len)
    {
        float m = 0.0f;
        for (std::size_t i = from; i < from + len; ++i)
            m = std::max(m, std::fabs(o.l[i]));
        return m;
    };
    const float p1 = peak(100 + 441, 441), p10 = peak(100 + 10 * 441, 441);
    CHECK(p1 > 0.45f);
    CHECK(p10 > 0.97f * p1); // lossless but for the 20 kHz LP and the 5 Hz-floor HP
    auto low = make<SendDelay>(fs, {{0, 0.0f}, {1, 1.0f}, {2, 0.3f}, {3, 0.5f}}); // hi cut 2.0 kHz glide, lo cut 647 Hz
    (void) run(*low, std::vector<float>(64 * 400, 0.0f), std::vector<float>(64 * 400, 0.0f));
    CHECK(low->hiCutHz() == Catch::Approx(2000.0 * std::pow(10.0, 299.0 / 999.0)).epsilon(1e-9));
    CHECK(low->loCutHz() == Catch::Approx(std::pow(1000.0, static_cast<double>(SendDelay::loCutStepOf(0.5f) - 1) / 999.0))
                                .epsilon(1e-9));
}

TEST_CASE("fx.delayBus: Sixteen fixes vs the Reference profile (FX-780/781/782)", "[fx][delayBus]")
{
    const double fs = 44100.0;
    SECTION("FX-780: an impulse at t = 0 echoes at D")
    {
        auto six = make<SendDelay>(fs, {{0, 490.0f / 3990.0f}, {1, 0.7f}, {2, 1.0f}});
        const std::size_t n = 64 * 400;
        const auto a = run(*six, impulse(n, 0), impulse(n, 0));
        CHECK(firstNonZero(a.l) == 22050);
    }
    SECTION("FX-781: no pre-reset audio after a reset with a time change")
    {
        for (auto q : {kQuirkProfileSixteen, kQuirkProfileReference})
        {
            auto e = make<SendDelay>(fs, {{0, 0.05f}, {1, 0.8f}, {2, 1.0f}}, q);
            const std::size_t n = 64 * 800;
            (void) run(*e, noise(n, 5u, n), noise(n, 6u, n));
            e->setParam(0, 0.2f);
            (void) run(*e, std::vector<float>(N, 0.0f), std::vector<float>(N, 0.0f)); // consumed before the reset
            e->reset();
            const auto o = run(*e, std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f));
            INFO("profile " << q);
            CHECK(silentFrom(o, 0) == (q == kQuirkProfileSixteen));
        }
    }
    SECTION("FX-782: the reset applies the stored time")
    {
        auto six = make<SendDelay>(fs, {{0, 40.0f / 3990.0f}}); // 50 ms, the module's constructor value
        CHECK(six->delaySamples() == 2205);
        CHECK(six->delayMs() == 50.0f);
        auto e = make<SendDelay>(fs, {{0, 1.0f}});
        CHECK(e->delaySamples() == 176400);
    }
}

// ---------------------------------------------------------------------------------------------------- catalogue

TEST_CASE("fx.delay / fx.delayBus: ParamSpecs, defaults and texts (FX-067/092/093/094)", "[fx][delay][delayBus]")
{
    const auto& reg = core::builtinParams();
    for (auto id : {"fx1.delay.time", "fx2.delay.feedback", "fx1.delay.hiCut", "fx1.delay.mix", "fx1.delay.sync",
                    "fx1.delay.time.div", "send.delayBus.time", "master.delayBus.spread", "send.delayBus.loCut",
                    "send.delayBus.sync", "master.delayBus.time.div", "send.reverb.preDelay", "master.reverb.damping"})
    {
        INFO(id);
        CHECK(reg.find(id) != nullptr);
    }
    CHECK(reg.find("fx1.delay.loCut") == nullptr); // F075: never read, no knob
    CHECK(reg.find("fx1.delay.time")->lockable);
    CHECK(! reg.find("send.delayBus.time")->lockable);
    CHECK(reg.find("fx1.delay.time")->defaultValue == 49.0f / 99.0f);
    CHECK(reg.find("fx1.delay.feedback")->defaultValue == 0.7f);
    CHECK(reg.find("fx1.delay.mix")->defaultValue == 0.0f);
    CHECK(reg.find("send.delayBus.mix")->defaultValue == 1.0f);
    CHECK(reg.find("master.delayBus.mix")->defaultValue == 0.5f);
    CHECK(reg.find("send.delayBus.time")->defaultValue == 490.0f / 3990.0f);
    char buf[32];
    reg.find("fx1.delay.time")->format(49.0f / 99.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "500 ms");
    reg.find("fx1.delay.time.div")->format(0.4f, buf, sizeof buf);
    CHECK(std::string(buf) == "1/2");
    reg.find("master.delayBus.time.div")->format(1.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "8/1");
    reg.find("fx1.delay.hiCut")->format(1.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "20.0 kHz");
    reg.find("send.delayBus.loCut")->format(0.25f, buf, sizeof buf);
    CHECK(std::string(buf) == "112 Hz");
    reg.find("send.delayBus.loCut")->format(0.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "OFF");
    reg.find("fx1.delay.feedback")->format(0.7f, buf, sizeof buf);
    CHECK(std::string(buf) == "70 %");
}

// ---------------------------------------------------------------------------------------------------- memory

TEST_CASE("fx.delay: memory per instance and of the 34-slot bank", "[fx][delay][memory]")
{
    std::size_t bank48 = 0;
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        InsertDelay ins;
        SendDelay bus;
        Reverb rev;
        ins.prepare(fs, N);
        bus.prepare(fs, N);
        rev.prepare(fs, N);
        // every type × slot pre-allocated (FX-005): 32 Insert Delays, a Send Delay and a Reverb on Send and Master
        const std::size_t total = 32 * ins.bytes() + 2 * bus.bytes() + 2 * rev.bytes();
        std::printf("fx.delay: %.0f Hz: Insert Delay %zu B (ring %d frames), Send Delay %zu B, Reverb %zu B per instance; "
                    "32 + 2 + 2 instances %.2f MiB\n",
                    fs, ins.bytes(), 46600 * static_cast<int>(fs / 44100.0 + 0.5), bus.bytes(), rev.bytes(),
                    static_cast<double>(total) / 1048576.0);
        if (fs == 48000.0)
            bank48 = total;
        const auto frames = static_cast<std::size_t>(static_cast<int>(1.0f * static_cast<float>(fs)) + 2500);
        CHECK(ins.bytes() == 2u * sizeof(float) * frames);
    }
    // FX buffer budget: ≤ 24 MiB at ≤ 48 kHz for the whole bank; the delays and the reverb take ≈ 16.5 MiB of it,
    // leaving room for the Chorus/Flanger rings (≈ 2.6 MB) and the small effects.
    CHECK(bank48 <= static_cast<std::size_t>(17.0 * 1048576.0));
}

TEST_CASE("fx.delayBus: 4000 ms with feedback 1 — two echoes at exact positions, full level (FX-720/783)",
          "[fx][delayBus]")
{
    // The golden (tests/golden/delayBus/t4000_fb1) holds one echo (the 2 MB file cap); this follows the research's
    // 5800-block run to the second one.
    auto e = make<SendDelay>(44100.0, {{0, 1.0f}, {1, 1.0f}, {2, 1.0f}, {3, 0.0f}, {4, 0.0f}});
    REQUIRE(e->delaySamples() == 176400);
    const std::size_t n = 64 * 5800, at = 4000;
    const auto o = run(*e, impulse(n, at), std::vector<float>(n, 0.0f));
    CHECK(firstNonZero(o.l) == static_cast<int>(at) + 176400);
    CHECK(firstNonZero(o.l, at + 176400 + 2000, 1e-3f) == static_cast<int>(at) + 2 * 176400);
    float p1 = 0.0f, p2 = 0.0f;
    for (std::size_t i = at + 176400; i < at + 176400 + 64; ++i)
        p1 = std::max(p1, std::fabs(o.l[i]));
    for (std::size_t i = at + 2 * 176400; i < at + 2 * 176400 + 64; ++i)
        p2 = std::max(p2, std::fabs(o.l[i]));
    CHECK(p2 > 0.75f * p1); // unity loop gain: only the 20 kHz LP smears the impulse a little more each pass
    CHECK(firstNonZero(o.r) < 0);
}

TEST_CASE("delay: digital retiming preserves a filled ring during rapid changes", "[fx][delay][delayBus]")
{
    // A constant ring isolates gain discontinuities from cancellation between unrelated taps.
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        auto sweep = [&](auto& e, float spanMs)
        {
            alignas(16) float l[N], r[N];
            const auto ctx = fxtest::context(fs, 120.0);
            const int fillBlocks = static_cast<int>(0.100 * fs / N) + 1;
            float maxError = 0.0f;
            for (int block = 0; block < fillBlocks + 450; ++block)
            {
                if (block >= fillBlocks)
                    e.setParam(0, static_cast<float>(block % 41) / spanMs);
                std::fill_n(l, N, 0.25f);
                std::fill_n(r, N, -0.5f);
                e.process(l, r, ctx);
                if (block >= fillBlocks)
                    for (int i = 0; i < N; ++i)
                        maxError = std::max({maxError, std::abs(l[i] - 0.25f), std::abs(r[i] + 0.5f)});
            }
            CHECK(maxError < 1e-4f);
        };
        // The bus's permanent low cut also removes DC, so test its shared mono reader directly.
        auto ins = make<InsertDelay>(fs, {{0, 0.0f}, {1, 0.0f}});
        sweep(*ins, 990.0f);
        dsp::delay::Module<1> mono;
        mono.prepare(fs, 4000.0f);
        mono.setDelay(10.0f);
        mono.reset(false, false);
        float input[N], output[N];
        std::fill_n(input, N, 0.25f);
        const float* in[] = {input};
        float* wet[] = {output};
        float worst = 0.0f;
        for (int block = 0; block < 800; ++block)
        {
            if (block >= 100)
                mono.setDelay(10.0f + static_cast<float>(block % 41));
            mono.read<false>(wet, N, false);
            mono.write(in, N);
            if (block >= 100)
                for (float v : output)
                    worst = std::max(worst, std::abs(v - 0.25f));
        }
        CHECK(worst < 1e-6f);
    }
}

TEST_CASE("delay: tape bends the playing echo in both directions, then recovers pitch", "[fx][delay][delayBus]")
{
    auto check = [](auto& e, float spanMs)
    {
        constexpr int fs = 48000;
        constexpr float hz = 400.0f;
        const auto ctx = fxtest::context(fs, 120.0);
        e.setParam(0, 240.0f / spanMs); // 250 ms
        e.setOption(e.kOptMode, 1);
        e.reset();
        alignas(16) float l[N], r[N];
        int t = 0;
        float last = 0;
        auto countCycles = [&](int blocks)
        {
            int cycles = 0;
            float maxStep = 0;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < N; ++i, ++t)
                    l[i] = r[i] = 0.3f * std::sin(static_cast<float>(2.0 * dsp::delay::kPi * hz * t / fs));
                e.process(l, r, ctx);
                for (float v : l)
                {
                    cycles += (v >= 0 && last < 0);
                    maxStep = std::max(maxStep, std::abs(v - last));
                    last = v;
                }
            }
            CHECK(maxStep < 0.05f);
            return cycles;
        };
        (void)countCycles(900);
        e.setParam(0, 40.0f / spanMs); // shorten to 50 ms
        CHECK(countCycles(75) > 65);   // unshifted: 40 cycles in this window
        (void)countCycles(650);
        e.setParam(0, 240.0f / spanMs);
        CHECK(countCycles(75) < 18);
        (void)countCycles(850); // wraps even the 1 s insert ring
        CHECK(std::abs(countCycles(75) - 40) <= 1);
        e.setOption(e.kOptMode, 0);
        CHECK(std::abs(countCycles(75) - 40) <= 1);
    };
    auto ins = make<InsertDelay>(48000, {{1, 0.0f}});
    auto bus = make<SendDelay>(48000, {{1, 0.0f}, {3, 0.0f}, {4, 0.0f}});
    check(*ins, 990.0f);
    check(*bus, 3990.0f);
}

TEST_CASE("delay: tape high frequencies decay faster on successive feedback repeats", "[fx][delay][delayBus]")
{
    auto check = [](auto& e)
    {
        // First difference energy / signal energy measures brightness independently of level.
        const std::size_t length = 64 * 180;
        const auto click = impulse(length, 300, 2.0f);
        const auto silence = std::vector<float>(length);
        auto capture = [&](int mode)
        {
            e.setOption(e.kOptMode, mode);
            e.reset();
            return run(e, click, silence, 120, 48000);
        };
        const auto digital = capture(0);
        const auto tape = capture(1);
        const auto brightness = [](const Out& o, int start)
        {
            double diff = 0, energy = 0;
            for (int i = start; i < start + 128; ++i)
            {
                const float v = o.l[i];
                diff += (v - o.l[i - 1]) * (v - o.l[i - 1]);
                energy += v * v;
            }
            REQUIRE(energy > 1e-6);
            return diff / energy;
        };
        const int d = e.delaySamples();
        CHECK(brightness(tape, 300 + d) < brightness(digital, 300 + d) * 0.8);
        CHECK(brightness(tape, 300 + 4 * d) < brightness(tape, 300 + d) * 0.6);
        CHECK(firstNonZero(tape.r) < 0);
        for (float x : tape.l)
            REQUIRE(std::abs(x) < 0.7f);
    };
    auto ins = make<InsertDelay>(48000, {{0, 0}, {1, 1}, {2, 1}});
    auto bus = make<SendDelay>(48000, {{0, 0}, {1, 1}, {2, 1}, {3, 0}, {4, 0}});
    check(*ins);
    check(*bus);
}
