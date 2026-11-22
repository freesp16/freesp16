// FX parameter model (core/params/catalogue/fx.h; spec parameters.md FX-061…094): the step law, the coarse multiplier,
// the CC map, the formats, the SYNC conversion against the research model (data/sync_convert.txt, every family at
// 20/60.5/87.5/120/133.37/999 BPM, both directions, every step: exact), the registration of effect ParamSpecs.
#include "core/params/catalogue/fx.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace core::fx;

namespace
{
const std::map<std::string, Family> kFamilies = {{"chorus", Family::Chorus},
                                                 {"flanger", Family::Flanger},
                                                 {"phaser", Family::Phaser},
                                                 {"insert_delay", Family::InsertDelay},
                                                 {"send_delay", Family::SendDelay},
                                                 {"ducker_attack", Family::DuckerAttack},
                                                 {"ducker_release", Family::DuckerRelease}};
}

TEST_CASE("fx.params: step law, round half to even, clamps at the grid ends (FX-063)", "[fx][params]")
{
    const Grid g{1, 101}; // chorus free: 100 steps
    for (int i = g.lo; i < g.hi; ++i)
    {
        const float v = valueOfStep(i, g);
        CHECK(stepIndex(v, g) == i);
        CHECK(step(v, g, +1, 1) == valueOfStep(std::min(i + 1, g.hi - 1), g));
        CHECK(step(v, g, -1, 1) == valueOfStep(std::max(i - 1, g.lo), g));
    }
    CHECK(step(0.0f, g, -1, 10) == 0.0f);
    CHECK(step(1.0f, g, +1, 10) == 1.0f);
    // half to even: grid [0, 3) span 2; v = 0.25 → 0.5 → 0, v = 0.75 → 1.5 → 2
    CHECK(stepIndex(0.25f, {0, 3}) == 0);
    CHECK(stepIndex(0.75f, {0, 3}) == 2);
    CHECK(valueOfStep(5, {0, 11}) == 0.5f);
}

TEST_CASE("fx.params: coarse multiplier (FX-064, our choice)", "[fx][params]")
{
    CHECK(coarseMultiplier(freeGrid(Family::Chorus), Family::Chorus, 0.3f, false, false) == 1);
    CHECK(coarseMultiplier(freeGrid(Family::Phaser), Family::Phaser, 0.3f, false, false) == 10);
    CHECK(coarseMultiplier(freeGrid(Family::Phaser), Family::Phaser, 0.3f, true, false) == 1);
    CHECK(coarseMultiplier(freeGrid(Family::Phaser), Family::Phaser, 0.3f, false, true) == 1);
    const Grid d = freeGrid(Family::SendDelay); // ms = step
    CHECK(coarseMultiplier(d, Family::SendDelay, valueOfStep(50, d), false, false) == 10);
    CHECK(coarseMultiplier(d, Family::SendDelay, valueOfStep(500, d), false, false) == 50);
    CHECK(coarseMultiplier(d, Family::SendDelay, valueOfStep(2000, d), false, false) == 100);
    const Grid a = freeGrid(Family::DuckerAttack); // 0.1 ms steps
    CHECK(coarseMultiplier(a, Family::DuckerAttack, valueOfStep(500, a), false, false) == 100);
    CHECK(coarseMultiplier(a, Family::DuckerAttack, valueOfStep(5000, a), false, false) == 500);
    CHECK(coarseMultiplier({0, 201}, Family::Plain, 0.5f, false, false) == 2);
}

TEST_CASE("fx.params: CC 0 and 127 hit the grid ends, 64 the middle (FX-086)", "[fx][params]")
{
    for (const Grid g : {Grid{0, 101}, Grid{1, 101}, Grid{0, 10}, Grid{10, 4001}, Grid{0, 128}})
    {
        CHECK(ccToValue(0, g) == 0.0f);
        CHECK(ccToValue(127, g) == 1.0f);
        const double x = 64.0 * g.span() / 127.0;
        CHECK(stepIndex(ccToValue(64, g), g) == g.lo + static_cast<int>(std::nearbyint(x)));
    }
}

TEST_CASE("fx.params: SYNC conversion equals the research model at every step (FX-073)", "[fx][params][sync]")
{
    std::ifstream in(std::string(APP_SOURCE_DIR) + "/tests/unit/fx/data/sync_convert.txt");
    REQUIRE(in.good());
    std::string line;
    int lines = 0, cases = 0;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        std::string fam;
        int toSync = 0;
        double bpm = 0;
        s >> fam >> toSync >> bpm;
        REQUIRE(kFamilies.count(fam) == 1);
        const Family f = kFamilies.at(fam);
        const Grid from = toSync ? freeGrid(f) : syncGrid(f);
        const Grid to = toSync ? syncGrid(f) : freeGrid(f);
        int i = from.lo;
        std::string run;
        while (s >> run)
        {
            const auto star = run.find('*');
            const int j = std::stoi(run.substr(0, star)), count = std::stoi(run.substr(star + 1));
            float expect = valueOfStep(j, to);
            expect = expect < 0.0f ? 0.0f : (expect > 1.0f ? 1.0f : expect);
            for (int c = 0; c < count; ++c, ++i)
            {
                const float v = valueOfStep(i, from);
                const float got = convertSync(v, f, toSync != 0, bpm);
                if (got != expect)
                {
                    INFO(fam << " toSync " << toSync << " bpm " << bpm << " step " << i << " → expected step " << j);
                    CHECK(got == expect);
                }
                ++cases;
            }
        }
        CHECK(i == from.hi);
        ++lines;
    }
    CHECK(lines == 7 * 2 * 6);
    CHECK(cases > 500000);
}

TEST_CASE("fx.params: SYNC off -> on -> off returns to a value that sat on a division (FX-074)", "[fx][params][sync]")
{
    const Grid g = freeGrid(Family::InsertDelay);
    const float v = valueOfStep(500, g); // 500 ms = 1 beat at 120 BPM
    const float s = convertSync(v, Family::InsertDelay, true, 120.0);
    CHECK(syncBeats(Family::InsertDelay, stepIndex(s, syncGrid(Family::InsertDelay))) == 1.0f);
    CHECK(convertSync(s, Family::InsertDelay, false, 120.0) == v);
}

TEST_CASE("fx.params: Ducker sync tables by formula (FX-1210)", "[fx][params]")
{
    CHECK(duckerSixteenths(Family::DuckerAttack, 0) == 0.0f);
    CHECK(duckerSixteenths(Family::DuckerAttack, 127) == 4.0f);
    CHECK(duckerSixteenths(Family::DuckerRelease, 0) == 0.25f);
    CHECK(duckerSixteenths(Family::DuckerRelease, 127) == 32.0f);
    for (int k = 1; k < 128; ++k)
    {
        CHECK(duckerSixteenths(Family::DuckerAttack, k) >= duckerSixteenths(Family::DuckerAttack, k - 1));
        CHECK(duckerSixteenths(Family::DuckerRelease, k) > duckerSixteenths(Family::DuckerRelease, k - 1));
    }
}

TEST_CASE("fx.params: value texts (FX-068)", "[fx][params]")
{
    char b[32];
    format(Family::Chorus, valueOfStep(15, freeGrid(Family::Chorus)), false, b, sizeof b);
    CHECK(std::string(b) == "1.5 Hz");
    format(Family::Flanger, valueOfStep(15, freeGrid(Family::Flanger)), false, b, sizeof b);
    CHECK(std::string(b) == "0.15 Hz");
    format(Family::Chorus, valueOfStep(5, syncGrid(Family::Chorus)), true, b, sizeof b);
    CHECK(std::string(b) == "3/4");
    format(Family::SendDelay, valueOfStep(250, freeGrid(Family::SendDelay)), false, b, sizeof b);
    CHECK(std::string(b) == "250 ms");
    format(Family::DuckerRelease, valueOfStep(125, freeGrid(Family::DuckerRelease)), false, b, sizeof b);
    CHECK(std::string(b) == "12.5 ms");
    format(Family::DuckerAttack, valueOfStep(127, syncGrid(Family::DuckerAttack)), true, b, sizeof b);
    CHECK(std::string(b) == "4.00");
    char small[4];
    CHECK(format(Family::SendDelay, 1.0f, false, small, sizeof small) == 3);
}

TEST_CASE("fx.params: registerEffect makes one ParamSpec per def and slot (FX-078/093/094)", "[fx][params]")
{
    static constexpr EffectParamDef kDefs[] = {{.key = "rate",
                                                .label = "RATE",
                                                .shortLabel = "RATE",
                                                .k = 0,
                                                .free = {1, 101},
                                                .sync = {0, 10},
                                                .family = Family::Chorus,
                                                .defInsert = 0.1f,
                                                .defSend = 0.2f,
                                                .defMaster = 0.3f},
                                               {.key = "mix",
                                                .label = "MIX",
                                                .shortLabel = "MIX",
                                                .k = 5,
                                                .free = {0, 101},
                                                .defInsert = 0.0f,
                                                .defSend = 1.0f,
                                                .defMaster = 0.5f}};
    static constexpr std::string_view kShape[] = {"SINE", "TRIANGLE"};
    static constexpr EffectOptionDef kOpts[] = {
        {.key = "shape", .label = "SHAPE", .shortLabel = "SHAPE", .enumLabels = kShape}};
    core::ParamRegistry r;
    registerEffect(r, "testfx", kDefs, kSlotInsert | kSlotSend | kSlotMaster, "fx.unit", kOpts);
    REQUIRE(r.errors().empty());
    CHECK(r.size() == 4 * 4); // fx1, fx2, send, master × {rate, rate.div (SYNC form), mix, shape}
    const auto* fx1 = r.find("fx1.testfx.rate");
    const auto* send = r.find("send.testfx.mix");
    const auto* master = r.find("master.testfx.mix");
    REQUIRE(fx1 != nullptr);
    REQUIRE(send != nullptr);
    REQUIRE(master != nullptr);
    CHECK(fx1->lockable);
    CHECK(fx1->lfoTarget);
    CHECK(fx1->scope == core::ParamScope::Track);
    CHECK_FALSE(send->lockable); // FX-094
    CHECK(send->scope == core::ParamScope::Scene);
    CHECK(fx1->defaultValue == 0.1f);
    CHECK(send->defaultValue == 1.0f); // FX-093: Send Mix 1.0, Master 0.5
    CHECK(master->defaultValue == 0.5f);
    CHECK(r.find("fx2.testfx.shape")->type == core::ParamType::Enum);
    char b[32];
    CHECK(fx1->formatValue(valueOfStep(15, {1, 101}), b, sizeof b) > 0);
    CHECK(std::string(b) == "1.5 Hz");
    const auto k = internalIndexTable(r);
    CHECK(k[r.idOf("fx2.testfx.mix")->value] == 5);
    CHECK(k[r.idOf("fx2.testfx.shape")->value] == -1);
    const auto* set = effectParams("testfx");
    REQUIRE(set != nullptr);
    CHECK(paramByK(*set, 5)->key == "mix");
    // The SYNC form (UID-830): `<id>.div` on the sync grid, the same knob k, the FX-068 sync text.
    const auto* div = r.find("fx1.testfx.rate.div");
    REQUIRE(div != nullptr);
    CHECK(div->label == "DIV");
    CHECK(div->lockable);
    CHECK(div->fineStep == 1.0f / 9.0f);
    CHECK(k[r.idOf("fx1.testfx.rate.div")->value] == 0);
    CHECK(div->formatValue(valueOfStep(5, {0, 10}), b, sizeof b) > 0);
    CHECK(std::string(b) == "3/4");
    CHECK(r.find("fx1.testfx.mix.div") == nullptr); // Plain: no SYNC form
    CHECK(r.find("send.testfx.rate.div")->scope == core::ParamScope::Scene);
}

TEST_CASE("fx.params: the built-in catalogue has the slot parameters and resolves effect ids", "[fx][params]")
{
    const auto& r = core::builtinParams();
    for (auto id : {"fx1.type", "fx2.type", "send.type", "master.type", "fx1.bypass", "fx2.bypass", "fx.mode",
                    "fx.quirkProfile", "fx.restartLfoOnPlay", "fx1.gainTest.gain", "master.none.mix"})
    {
        INFO(id);
        CHECK(r.find(id) != nullptr);
    }
    CHECK(r.find("fx1.gainTest.gain")->featureFlag == "fx.test");
    CHECK(r.find("send.type")->scope == core::ParamScope::Scene);
    for (int t = 1; t <= static_cast<int>(core::model::EffectType::Ducker); ++t)
    {
        const auto type = static_cast<core::model::EffectType>(t);
        CHECK(effectTypeOf(effectIdOf(type)) == type);
    }
    CHECK(effectIdOf(core::model::EffectType::Off).empty());
}
