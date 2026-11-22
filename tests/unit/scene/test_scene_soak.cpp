// 1 h of 64-frame blocks (2.48 M at 44.1 kHz) through the switcher alone, a switch requested every bar with the STEP
// unit (the bench_scene scenario), on a thread marked real-time: no allocation (APP_RT_CHECKS aborts), every switch on
// the exact frame of its step boundary, no drift.
#include "core/util/Realtime.h"
#include "tests/unit/scene/SceneTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
struct CountingTransport
{
    int tempos = 0;
    void command(const engine::TransportCmd&) noexcept { ++tempos; }
};
struct CountingMixer
{
    int scenes = 0, masks = 0;
    void setScene(int, int) noexcept { ++scenes; }
    void setPatternMuteSolo(std::uint16_t, std::uint16_t, int) noexcept { ++masks; }
};
struct CountingEvents
{
    int events = 0;
    bool pushEvent(const engine::EngineEvent&) noexcept { return ++events > 0; }
};
} // namespace

TEST_CASE("scene.soak: 1 h of blocks, a STEP-unit switch every bar, exact frames, no allocation", "[scene][soak]")
{
    engine::scene::ScenePatternSwitcher sw;
    sw.setUnit(engine::scene::SwitchUnit::Step);
    for (int s = 0; s < 16; ++s)
    {
        sw.setSceneBpm(s, 0.0f);
        sw.setPatternLength(s, 1, 8 + 4 * s);
    }
    scenetest::Clock clock;
    clock.play();
    engine::stages::BlockBuffers::Timing timing {};
    CountingTransport transport;
    CountingMixer mixer;
    CountingEvents events;
    const std::int64_t blocks = 3600LL * 44100LL / engine::kBlockFrames;
    const double ppf = clock.ppqPerFrame();
    int requested = 0, wrong = 0;
    double nextBar = 0.0;
    {
        core::rt::ScopedRealtimeThread rt;
        for (std::int64_t b = 0; b < blocks; ++b)
        {
            const auto ctx = clock.next();
            if (ctx.ppqStart >= nextBar)
            {
                const int target = (requested % 16) * 16 + 1 + (requested & 1) * 2;
                sw.request(target / 16, target % 16);
                ++requested;
                nextBar += 4.0;
            }
            const auto r = sw.process(ctx, timing, transport, mixer, events);
            if (r.switched)
            {
                // The step boundary at or after the request's block start, as an absolute frame.
                const double boundary = timing.boundary.ppq;
                const auto expected = static_cast<std::int64_t>(boundary / ppf + 1e-6);
                if (b * engine::kBlockFrames + r.frame != expected || std::fmod(boundary, 0.25) > 1e-9)
                    ++wrong;
            }
        }
    }
    CHECK(wrong == 0);
    CHECK(events.events == requested);
    CHECK(mixer.scenes == requested - 1); // the first request stays in scene 1 (pattern 2)
    CHECK(transport.tempos == 0);
    CHECK(requested == 1800);
}
