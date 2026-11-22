// Input stage and threshold kernels (SMP-002/003/004, MIX-021): the threshold table, the first-crossing
// kernel (scalar reference; NEON parity bit for bit where NEON exists — SKIPPED on x86, run under qemu by
// scripts/neon-tests.sh), the soft limiter on/off on the input and the meter / clip flags, and the catalogue.
#include "core/params/ParamRegistry.h"
#include "core/util/Realtime.h"
#include "engine/sampling/InputStage.h"
#include "engine/sampling/SamplingStage.h"
#include "engine/sampling/Threshold.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

using namespace engine::sampling;

TEST_CASE("sampling.threshold: −60…−20 dB in 1 dB steps, OFF and PLAY have no level", "[sampling][threshold]")
{
    CHECK(thresholdGain(0) == 0.0f);
    CHECK(thresholdGain(42) == 0.0f);
    CHECK(thresholdDb(1) == -60);
    CHECK(thresholdDb(41) == -20);
    for (int i = 1; i <= 41; ++i)
        CHECK(thresholdGain(i) == Catch::Approx(std::pow(10.0, (i - 61) / 20.0)).epsilon(1e-5));
}

TEST_CASE("sampling.threshold: first frame strictly above the threshold on either channel", "[sampling][threshold]")
{
    float l[64] {}, r[64] {};
    const float thr = thresholdGain(21); // −40 dB
    CHECK(scalar::firstAbove(l, r, 64, thr) == -1);
    l[37] = thr; // equal is not above
    CHECK(scalar::firstAbove(l, r, 64, thr) == -1);
    r[41] = -1.01f * thr;
    CHECK(scalar::firstAbove(l, r, 64, thr) == 41);
    l[5] = 0.5f;
    CHECK(scalar::firstAbove(l, r, 64, thr) == 5);
    CHECK(scalar::firstAbove(l, nullptr, 64, thr) == 5);
    CHECK(firstAbove(l, r, 64, thr) == 5);
}

TEST_CASE("sampling.threshold: NEON first-crossing equals the scalar reference", "[sampling][threshold][neon]")
{
#if APP_SAMPLING_HAVE_NEON
    core::rt::enableFlushToZero();
    std::uint32_t s = 12345;
    float l[67], r[67];
    for (int trial = 0; trial < 2000; ++trial)
    {
        for (int i = 0; i < 67; ++i)
        {
            s = s * 1664525u + 1013904223u;
            l[i] = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.01f;
            s = s * 1664525u + 1013904223u;
            r[i] = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.01f;
        }
        const int n = 1 + trial % 67;
        const float thr = thresholdGain(1 + trial % 41);
        REQUIRE(neon::firstAbove(l, r, n, thr) == scalar::firstAbove(l, r, n, thr));
    }
#else
    SKIP("no NEON on this host (scripts/neon-tests.sh runs it under qemu-arm)");
#endif
}

TEST_CASE("sampling.input: limiter OFF passes the raw input; ON shapes only above −3 dBFS", "[sampling][input]")
{
    engine::stages::BlockBuffers b {};
    alignas(16) float inL[64], inR[64];
    for (int i = 0; i < 64; ++i)
    {
        inL[i] = 0.5f * std::sin(0.3f * static_cast<float>(i));
        inR[i] = -inL[i];
    }
    InputStage st;
    b.inputL = inL;
    b.inputR = inR;
    st.process(b);
    CHECK(b.inputL == inL); // OFF: no copy
    CHECK(st.peak(0) == Catch::Approx(0.5f).margin(0.01));
    CHECK_FALSE(st.clipped());

    st.setLimiter(true);
    st.process(b);
    CHECK(b.inputL == inL); // ON, but quiet (−6 dBFS < −3 dBFS): the shaper is identity, no copy

    for (int i = 0; i < 64; ++i)
        inL[i] = inR[i] = (i % 2 ? 1.0f : -1.0f); // full scale: clipped at the input
    b.inputL = inL;
    b.inputR = inR;
    st.process(b);
    REQUIRE(b.inputL != inL);
    const float ceiling = std::pow(10.0f, -0.3f / 20.0f);
    for (int i = 0; i < 64; ++i)
    {
        CHECK(std::fabs(b.inputL[i]) <= ceiling);
        CHECK(std::fabs(b.inputL[i]) > 0.9f);
    }
    CHECK(inL[0] == -1.0f); // the raw block is untouched
    CHECK(st.clipped());
    CHECK(st.peak(0) <= ceiling);

    b.inputL = b.inputR = nullptr;
    st.process(b);
    CHECK(st.peak(0) == 0.0f);
    CHECK(b.inputL == nullptr);
}

TEST_CASE("sampling.params: the catalogue registers sampling.* and live.* with their ranges", "[sampling][params]")
{
    const auto& reg = core::builtinParams();
    for (const auto id : kSamplingParamIds)
    {
        INFO(id);
        REQUIRE(reg.find(id) != nullptr);
    }
    const auto* th = reg.find("sampling.threshold");
    char buf[16];
    th->format(0.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "OFF");
    th->format(1.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "-60dB");
    th->format(41.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "-20dB");
    th->format(42.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "PLAY");
    CHECK(reg.find("sampling.length")->maxValue == 4.0f);
    CHECK(reg.find("live.length")->enumLabels.size() == 6);
    CHECK(reg.find("sampling.rec")->scope == core::ParamScope::Track);
}
