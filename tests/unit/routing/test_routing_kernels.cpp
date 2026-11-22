// The routing block kernels (engine/mixer/OutputRouterKernels.h): the scalar references against their
// definitions, and the NEON kernels against the scalar references bit for bit (__ARM_NEON only: on x86 the parity
// cases SKIP; scripts/neon-tests.sh runs them under qemu-arm).
#include "tests/unit/routing/RoutingTestUtil.h"

#include "engine/mixer/OutputRouterKernels.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace k = engine::mixer::routing;
using routingtest::kN;

namespace
{
struct Buf
{
    alignas(16) float a[kN], b[kN], g[kN], d[kN];
};

void fill(Buf& x, routingtest::Noise& n)
{
    for (int i = 0; i < kN; ++i)
    {
        x.a[i] = n.next(0.9f);
        x.b[i] = n.next(0.9f);
        x.g[i] = 0.5f + 0.5f * n.next();
        x.d[i] = n.next(0.5f);
    }
}

[[maybe_unused]] bool same(const float* a, const float* b) { return std::memcmp(a, b, sizeof(float) * kN) == 0; }
} // namespace

TEST_CASE("routing.kernels: the scalar references compute their definitions (one rounding per operation)", "[routing]")
{
    routingtest::Noise n;
    Buf x;
    fill(x, n);
    float d[kN];
    std::memcpy(d, x.d, sizeof d);
    k::scalar::addMono(d, x.a, x.b, kN);
    for (int i = 0; i < kN; ++i)
    {
        const float s = x.a[i] + x.b[i];
        CHECK(d[i] == x.d[i] + s * 0.5f);
    }
    std::memcpy(d, x.d, sizeof d);
    k::scalar::addMonoGain(d, x.a, x.b, x.g, kN);
    for (int i = 0; i < kN; ++i)
    {
        const float s = (x.a[i] + x.b[i]) * 0.5f;
        const float p = s * x.g[i];
        CHECK(d[i] == x.d[i] + p);
    }
    std::memcpy(d, x.d, sizeof d);
    k::scalar::addGain(d, x.a, x.g, kN);
    for (int i = 0; i < kN; ++i)
    {
        const float p = x.a[i] * x.g[i];
        CHECK(d[i] == x.d[i] + p);
    }
    k::scalar::copyScale(d, x.a, 0.75f, kN);
    for (int i = 0; i < kN; ++i)
        CHECK(d[i] == x.a[i] * 0.75f);
}

TEST_CASE("routing.kernels: the selector fade is the research Selector's: (128 - k) / 128 out, k / 128 in, exact",
          "[routing]")
{
    float g[kN];
    k::SelectorFade f;
    CHECK(f.idle());
    f.setTarget(0.0f); // fade-out from 1 (headphone_mix.Selector, F209/F212)
    f.fill(g, kN);
    CHECK(g[0] == 1.0f);
    CHECK(g[63] == 65.0f / 128.0f);
    CHECK_FALSE(f.idle());
    f.fill(g, kN);
    CHECK(g[0] == 0.5f);
    CHECK(g[63] == 1.0f / 128.0f); // the last fade-out frame
    CHECK(f.idle());
    CHECK(f.current() == 0.0f);
    f.setTarget(0.0f); // the same target: nothing
    CHECK(f.idle());
    f.setTarget(1.0f); // fade-in from 0
    f.fill(g, kN);
    CHECK(g[0] == 0.0f); // the first fade-in frame
    CHECK(g[1] == 1.0f / 128.0f);
    f.setTarget(0.0f); // a change half-way through the fade-in: 128 frames down from the gain reached (0.5)
    f.fill(g, kN);
    CHECK(g[0] == 0.5f);
    CHECK(g[1] == 0.5f * 127.0f / 128.0f);
    f.fill(g, kN);
    CHECK(g[63] == 0.5f / 128.0f);
    CHECK(f.idle());
    f.fill(g, kN);
    CHECK(g[0] == 0.0f);
    CHECK(g[63] == 0.0f);
}

TEST_CASE("routing.kernels: NEON = scalar bit for bit (__ARM_NEON)", "[routing][neon]")
{
#if APP_ROUTING_HAVE_NEON
    routingtest::Noise n;
    for (int rep = 0; rep < 50; ++rep)
    {
        Buf x;
        fill(x, n);
        float s[kN], v[kN];
        const auto check = [&](auto scalarFn, auto neonFn) {
            std::memcpy(s, x.d, sizeof s);
            std::memcpy(v, x.d, sizeof v);
            scalarFn(s);
            neonFn(v);
            CHECK(same(s, v));
        };
        check([&](float* d) { k::scalar::add(d, x.a, kN); }, [&](float* d) { k::neon::add(d, x.a, kN); });
        check([&](float* d) { k::scalar::addGain(d, x.a, x.g, kN); }, [&](float* d) { k::neon::addGain(d, x.a, x.g, kN); });
        check([&](float* d) { k::scalar::addMono(d, x.a, x.b, kN); }, [&](float* d) { k::neon::addMono(d, x.a, x.b, kN); });
        check([&](float* d) { k::scalar::addMonoGain(d, x.a, x.b, x.g, kN); },
              [&](float* d) { k::neon::addMonoGain(d, x.a, x.b, x.g, kN); });
        check([&](float* d) { k::scalar::copyGain(d, x.a, x.g, kN); }, [&](float* d) { k::neon::copyGain(d, x.a, x.g, kN); });
        check([&](float* d) { k::scalar::copyScale(d, x.a, 0.3f, kN); }, [&](float* d) { k::neon::copyScale(d, x.a, 0.3f, kN); });
        check([&](float* d) { k::scalar::multiply(d, x.g, kN); }, [&](float* d) { k::neon::multiply(d, x.g, kN); });
    }
#else
    SKIP("no NEON on this host (x86): the parity runs under qemu-arm (scripts/neon-tests.sh)");
#endif
}
