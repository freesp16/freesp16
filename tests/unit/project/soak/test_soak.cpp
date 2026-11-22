// NFR-014 soak driver (not in CI). Hidden Catch2 test: ctest never lists
// it; run it by tag, for as long as SOAK_SECONDS says (default 24 h):
//     SOAK_SECONDS=86400 build/host-release/tests/<prefix>_tests_project "[.soak]"
// The real engine renders offline as fast as it can (one simulated day takes far less than a day on the host) while
// the driver loops scene/pattern switches (scene.queue, when it is registered), automated parameter
// changes on every track, pad hits on all 16 tracks and autosaves of a changing project. Checks at the end: no
// missed block (xruns = 0 for an offline device), resident memory growth ≤ 10 MB after the warm-up, and no stuck
// voice: after the hits stop, the output decays to silence.
#include "../ProjectTestSupport.h"

#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"
#include "tests/fakes/FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace
{
std::int64_t residentBytes()
{
#if defined(__APPLE__)
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<std::int64_t>(info.resident_size);
#else
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r"))
    {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return static_cast<std::int64_t>(resident) * 4096;
#endif
}
} // namespace

TEST_CASE("project.soak: 24 h of scene switches and automated parameters (NFR-014)", "[.soak]")
{
    const char* soakSeconds = std::getenv("SOAK_SECONDS");
    const double seconds = soakSeconds != nullptr ? std::atof(soakSeconds) : 86400.0;
    auto& registry = core::builtinParams();
    engine::EngineSetup setup;
    setup.resolveParam = [&registry](std::string_view id) { return registry.idOf(id).value_or(core::ParamId {}); };
    auto engine = engine::createEngine(setup);
    fakes::FakeAudioDevice device;
    std::string info;
    platform::AudioConfig config;
    REQUIRE(device.start(config, *engine, info));
    const auto params = std::vector<core::ParamId> {registry.idOf("track.volume").value_or(core::ParamId {}),
                                                    registry.idOf("track.pan").value_or(core::ParamId {})};
    const auto sceneQueue = registry.idOf("scene.queue");

    // The idle output before any hit (the fallback test tone of track 1 plays until the sample voices replace it).
    for (int i = 0; i < 8; ++i)
        REQUIRE(device.runPeriods(1));
    float idlePeak = 0;
    for (const float v : device.mainLeft())
        idlePeak = std::max(idlePeak, std::fabs(v));
    device.clearCapture();

    projtest::Env env("soak");
    env.startup();
    REQUIRE(env.store().create("Soak").ok());

    const double rate = config.sampleRate;
    const auto totalPeriods = static_cast<std::uint64_t>(seconds * rate / config.periodFrames);
    std::uint32_t x = 1;
    const auto rnd = [&x]
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    };
    std::int64_t baseline = 0;
    std::uint64_t refused = 0;
    for (std::uint64_t p = 0; p < totalPeriods; ++p)
    {
        auto& bridge = engine->bridge();
        if (p % 8 == 0)
            for (int t = 0; t < 16; ++t)
                for (const auto id : params)
                    if (id.valid() && ! bridge.post(engine::ParamChange {id, static_cast<std::uint16_t>(t),
                                                                         static_cast<float>(rnd() % 128), 64}))
                        ++refused;
        if (p % 172 == 0 && sceneQueue) // ≈ every second: queue another scene/pattern
            bridge.post(engine::ParamChange {*sceneQueue, 0xFFFF, static_cast<float>(rnd() % 256), 0});
        if (p % 3 == 0)
            engine->triggers().push({static_cast<std::uint8_t>(rnd() % 16), 100, 60, -1});
        REQUIRE(device.runPeriods(1));
        device.clearCapture();
        if (p % 64 == 0)
            bridge.retirePending();
        if (p % 17200 == 0) // ≈ every 100 s of audio: an edit + autosave + housekeeping
        {
            projtest::setTempo(env.bus, 60.0 + static_cast<double>(rnd() % 120));
            env.session->autosave().writeNow();
            env.workers.drain();
            env.pool.collect();
        }
        if (p == totalPeriods / 20)
            baseline = residentBytes(); // after the warm-up
    }
    // No stuck voices: one second without hits decays to silence.
    for (int i = 0; i < 2 * static_cast<int>(rate / config.periodFrames); ++i)
        REQUIRE(device.runPeriods(1));
    device.clearCapture();
    REQUIRE(device.runPeriods(8));
    float peak = 0;
    for (const float v : device.mainLeft())
        peak = std::max(peak, std::fabs(v));
    const auto load = engine->load();
    const auto growth = residentBytes() - baseline;
    INFO("periods " << totalPeriods << ", refused posts " << refused << ", RSS growth " << growth / 1024 << " KB, peak "
                    << peak << " (idle " << idlePeak << "), engine blocks " << engine->blocksRendered());
    CHECK(engine->blocksRendered() >= totalPeriods * static_cast<std::uint64_t>(config.periodFrames / 64));
    CHECK(growth <= 10ll * 1024 * 1024);
    CHECK(peak <= idlePeak * 1.01f + 1.0e-4f); // back to the idle output: no voice left sounding
    (void) load;
}
