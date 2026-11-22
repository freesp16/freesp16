// Zero-crossing snap for slice points (SLC-006: ±5 ms): the nearest sign change, ties to the earlier
// frame, stereo = L + R, nothing in the window = the frame itself; the NEON scan equals the scalar reference (SKIPs on
// x86-64; scripts/neon-tests.sh runs it under qemu).
#include "core/util/Realtime.h"
#include "services/analysis/SliceDetect.h"
#include "tests/unit/voice/VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>

namespace sa = services::analysis;
using voicetest::TestBuffer;

TEST_CASE("slice.detect: the nearest zero crossing within the window, else the frame itself", "[slice]")
{
    std::vector<float> v(1000, 0.5f);
    for (int i = 300; i < 600; ++i)
        v[static_cast<std::size_t>(i)] = -0.5f; // crossings at 300 (+ → −) and 600 (− → +)
    TestBuffer b(v);
    CHECK(sa::nearestZeroCrossing(*b.get(), 310, 50) == 300);
    CHECK(sa::nearestZeroCrossing(*b.get(), 590, 50) == 600);
    CHECK(sa::nearestZeroCrossing(*b.get(), 450, 150) == 300); // tie → the earlier
    CHECK(sa::nearestZeroCrossing(*b.get(), 100, 50) == 100);  // none in reach
    CHECK(sa::nearestZeroCrossing(*b.get(), 5000, 50) == 999); // clamped
    CHECK(sa::windowFrames(*b.get()) == 221);                  // ±5 ms at 44.1 kHz

    std::vector<float> l(1000, 0.25f), r(1000, 0.25f);
    for (int i = 500; i < 1000; ++i)
        r[static_cast<std::size_t>(i)] = -0.75f; // the sum crosses at 500 though L never does
    TestBuffer st(l, r);
    CHECK(sa::nearestZeroCrossing(*st.get(), 520, 100) == 500);
}

TEST_CASE("slice.detect: NEON sign-change scan equals the scalar reference", "[slice][simd]")
{
#if defined(__ARM_NEON)
    core::rt::enableFlushToZero();
    std::vector<float> v = voicetest::sine(44100, 437.0);
    TestBuffer b(v), s(v, voicetest::sine(44100, 91.0));
    for (std::int64_t f = 0; f < 44100; f += 97)
        for (const int w : {0, 3, 64, 221, 2000})
        {
            REQUIRE(sa::nearestZeroCrossingNeon(*b.get(), f, w) == sa::nearestZeroCrossingScalar(*b.get(), f, w));
            REQUIRE(sa::nearestZeroCrossingNeon(*s.get(), f, w) == sa::nearestZeroCrossingScalar(*s.get(), f, w));
        }
#else
    SKIP("no NEON on this host (scripts/neon-tests.sh runs it under qemu-arm)");
#endif
}
