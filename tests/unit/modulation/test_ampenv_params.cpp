// The env.* ParamSpecs (UID-200/201; ENV-005/010/011; CC 26–29) and the engine's applyParam mapping.
#include "core/params/ParamRegistry.h"
#include "engine/modulation/AmpEnvelopeParams.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("ampenv.params: catalogue entries, defaults, CCs and locks", "[ampenv]")
{
    const auto& r = core::builtinParams();
    REQUIRE(r.errors().empty());
    struct Row
    {
        const char* id;
        float max, def;
        int cc;
        bool lockable;
    };
    for (const Row& row :
         {Row{"env.velocity", 127, 127, 26, true}, Row{"env.attack", 127, 0, 27, true},
          Row{"env.hold", 128, 128, 28, true}, Row{"env.release", 128, 0, 29, true}, Row{"env.sync", 1, 0, -1, false},
          Row{"env.attackCurve", 1, 0, -1, false}, Row{"env.releaseCurve", 1, 0, -1, false},
          Row{"env.retrigger", 1, 0, -1, false}, Row{"env.bypass", 1, 0, -1, false}})
    {
        INFO(row.id);
        const auto* s = r.find(row.id);
        REQUIRE(s != nullptr);
        CHECK(s->maxValue == row.max);
        CHECK(s->defaultValue == row.def);
        CHECK(s->midiCc == row.cc);
        CHECK(s->lockable == row.lockable);
        CHECK(s->scope == core::ParamScope::Track);
        if (row.cc >= 0)
            CHECK(r[*r.byMidiCc(row.cc)].id == row.id);
    }
    CHECK(r.find("env.velocity")->stripTarget);
    // every id the engine binds exists (track.padVelocity comes from track.cpp)
    for (const auto& p : engine::modulation::kAmpEnvParamIds)
        if (p.id.substr(0, 4) == "env.")
            CHECK(r.find(p.id) != nullptr);
}

TEST_CASE("ampenv.params: captions of the time values", "[ampenv]")
{
    const auto& r = core::builtinParams();
    char buf[32];
    auto fmt = [&](const char* id, float v)
    {
        const int n = r.find(id)->formatValue(v, buf, sizeof buf);
        return std::string(buf, static_cast<std::size_t>(n));
    };
    CHECK(fmt("env.attack", 64) == "132 ms");
    CHECK(fmt("env.attack", 127) == "32.0 s");
    CHECK(fmt("env.hold", 128) == "INF");
    CHECK(fmt("env.release", 128) == "INF");
    CHECK(fmt("env.release", 0) == "0 ms");
    CHECK(fmt("env.attackCurve", 1) == "CURVE");
    CHECK(fmt("env.retrigger", 1) == "ON");
}

TEST_CASE("ampenv.params: applyParam writes native values into the block", "[ampenv]")
{
    using namespace engine::modulation;
    AmpEnvParams p;
    CHECK_FALSE(applyParam(p, AmpEnvField::Hold, 128));
    CHECK(applyParam(p, AmpEnvField::Hold, 40.4f));
    CHECK(p.hold == 40);
    CHECK(applyParam(p, AmpEnvField::Attack, 300));
    CHECK(p.attack == 127);
    CHECK(applyParam(p, AmpEnvField::Release, 999));
    CHECK(p.release == 128);
    CHECK(applyParam(p, AmpEnvField::Velocity, 64));
    CHECK(p.velocity == 64);
    CHECK(applyParam(p, AmpEnvField::ReleaseCurve, 1));
    CHECK(p.releaseCurve == EnvCurve::Curve);
    CHECK(applyParam(p, AmpEnvField::VelocitySensitive, 0));
    CHECK_FALSE(p.velocitySensitive);
    CHECK(applyParam(p, AmpEnvField::Sync, 1));
    CHECK(applyParam(p, AmpEnvField::Retrigger, 1));
    CHECK(applyParam(p, AmpEnvField::Bypass, 1));
    CHECK(p.sync);
    CHECK(p.retrigger);
    CHECK(p.bypass);
}
