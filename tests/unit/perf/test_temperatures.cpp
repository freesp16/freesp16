#include "services/library/Workers.h"
#include "services/system/Temperatures.h"
#include "tests/unit/library/LibTestAudio.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("temperature monitor reads the enabled thermal zones on a worker", "[temperature]")
{
    libtest::TempDir root("thermal");
    services::library::Workers workers(1);
    const auto path = root.dir().getFullPathName().toStdString();
    const auto zone = [&](const char* name, const char* type, const char* reading)
    {
        const auto dir = root / name;
        REQUIRE(dir.createDirectory().wasOk());
        REQUIRE(dir.getChildFile("type").replaceWithText(type));
        REQUIRE(dir.getChildFile("temp").replaceWithText(reading));
    };

    zone("thermal_zone3", "anatoptz", "47\n");
    services::system::TemperatureMonitor unit(workers, path);
    const auto die = unit.available();
    CHECK_FALSE(die.cpu);
    CHECK_FALSE(die.gpu);
    REQUIRE(die.soc);
    unit.poll(die, 0);
    workers.drain();
    CHECK(unit.poll(die, 1).soc == 47);
    zone("thermal_zone7", "cpu-thermal", "51500\n");
    const services::system::TemperatureSelection cpuOnly{true, false, false};
    CHECK(services::system::TemperatureMonitor(workers, path).available() == cpuOnly);
    zone("thermal_zone8", "gpu-thermal", "400\n");
    zone("thermal_zone0", "board", "87000\n");
    zone("cooling_device0", "gpu", "68000\n");
    const services::system::TemperatureSelection both{true, true}, gpu{false, true};
    services::system::TemperatureMonitor monitor(workers, path);
    REQUIRE(monitor.available() == both);
    monitor.poll({}, 0);
    CHECK(workers.idle());
    monitor.poll(both, 1);
    workers.drain();
    const auto first = monitor.poll(both, 2);
    CHECK(first.cpu == 52);
    CHECK(first.gpu == 0);
    CHECK_FALSE(first.soc);
    zone("thermal_zone8", "gpu-thermal", "51000");
    CHECK(monitor.poll(both, 2000).gpu == 0);
    CHECK(workers.idle());
    monitor.poll(both, 2001);
    workers.drain();
    CHECK(monitor.poll(both, 2002).gpu == 51);

    zone("thermal_zone8", "gpu-thermal", "55oops");
    monitor.poll(gpu, 2003);
    workers.drain();
    const auto bad = monitor.poll(gpu, 2004);
    CHECK_FALSE(bad.cpu);
    CHECK_FALSE(bad.gpu);
    CHECK_FALSE(bad.soc);

    monitor.poll(both, 2005); // switch off with a read outstanding
    monitor.poll({}, 2006);
    workers.drain();
    const auto off = monitor.poll({}, 2007);
    CHECK_FALSE(off.cpu);
    CHECK_FALSE(off.gpu);
    CHECK(workers.idle());
    REQUIRE((root / "thermal_zone3").deleteRecursively());
    zone("thermal_zone8", "gpu-thermal", "999999");
    monitor.poll(gpu, 2008);
    workers.drain();
    const auto missing = monitor.poll(gpu, 2009);
    CHECK_FALSE(missing.cpu);
    CHECK_FALSE(missing.gpu);
    CHECK_FALSE(missing.soc);
}
