// Model paths ↔ ParamPath via the ParamRegistry (data-model.md "Identity and paths"): every registered
// ParamSpec has a path that round-trips, for every scope form of the grammar in core/model/paths/ParamPath.cpp.
#include "core/model/Pattern.h"
#include "core/model/paths/ParamPath.h"
#include "core/params/ParamRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using namespace core::model;
using core::ParamRegistry;
using core::ParamScope;
using core::ParamSpec;

namespace
{
ParamSpec spec(std::string_view id, ParamScope scope, bool lockable = false)
{
    ParamSpec s;
    s.id = id;
    s.label = "LABEL";
    s.scope = scope;
    s.lockable = lockable;
    return s;
}

// A registry with one spec of every path form.
ParamRegistry& formsRegistry()
{
    static ParamRegistry r = [] {
        ParamRegistry reg;
        reg.add(spec("track.volume", ParamScope::Track, true));
        reg.add(spec("playback.pitch", ParamScope::Track, true));
        reg.add(spec("track.colour", ParamScope::Track, false));
        reg.add(spec("fx1.chorus.rate", ParamScope::Track, true));
        reg.add(spec("fx2.chorus.rate", ParamScope::Track, true));
        reg.add(spec("send.reverb.size", ParamScope::Scene));
        reg.add(spec("master.compressor.ratio", ParamScope::Scene));
        reg.add(spec("pattern.length", ParamScope::Scene));
        reg.add(spec("scene.colour", ParamScope::Scene));
        reg.add(spec("routing.fxMode", ParamScope::Scene));
        reg.add(spec("scene.bpm", ParamScope::SceneOverrideElseProject));
        reg.add(spec("project.tempo", ParamScope::Project));
        reg.add(spec("arrangement.play", ParamScope::Project));
        reg.add(spec("settings.padCurve", ParamScope::Global));
        return reg;
    }();
    return r;
}

core::ParamId idOf(const ParamRegistry& r, std::string_view id)
{
    auto v = r.idOf(id);
    REQUIRE(v.has_value());
    return *v;
}

// Every path the spec can have at a few index combinations; each must parse back to the same ParamPath and format
// to the same text. Returns every distinct text with the index of its spec.
std::map<std::string, std::size_t> roundTripAll(const ParamRegistry& r)
{
    std::map<std::string, std::size_t> texts;
    const std::uint8_t idx[][5] = {{0, 0, 0, 0, 0}, {15, 15, 15, 63, 0}, {3, 7, 2, 10, 0}};
    const auto all = r.all();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const core::ParamId id {static_cast<std::uint16_t>(i)};
        int formatted = 0;
        for (auto scope : {Scope::Project, Scope::Scene, Scope::Track, Scope::Pattern, Scope::Step, Scope::InsertFx,
                           Scope::SendFx, Scope::MasterFx})
            for (const auto& ix : idx)
                for (std::uint8_t slot : {0, 1})
                {
                    const ParamPath p {scope, ix[0], ix[1], ix[2], ix[3], slot, id};
                    const auto text = formatPath(p, r);
                    if (text.empty())
                        continue;
                    ++formatted;
                    INFO(text);
                    const auto back = parsePath(text, r);
                    REQUIRE(back.has_value());
                    CHECK(back->scope == p.scope);
                    CHECK(back->param == p.param);
                    CHECK(formatPath(*back, r) == text);
                    // indices the scope uses survive
                    if (scope != Scope::Project)
                        CHECK(back->scene == p.scene);
                    if (scope == Scope::Track || scope == Scope::InsertFx || scope == Scope::Step)
                        CHECK(back->track == p.track);
                    if (scope == Scope::Pattern || scope == Scope::Step)
                        CHECK(back->pattern == p.pattern);
                    if (scope == Scope::Step)
                        CHECK(back->step == p.step);
                    if (scope == Scope::InsertFx)
                        CHECK(back->slot == p.slot);
                    const auto [it, inserted] = texts.emplace(text, i);
                    CHECK((inserted || it->second == i)); // no two specs share a path
                }
        INFO(std::string(all[i].id));
        CHECK(formatted > 0); // every registered spec has at least one path
    }
    return texts;
}
} // namespace

TEST_CASE("paths of every scope form parse and format", "[model][path]")
{
    const auto& r = formsRegistry();
    REQUIRE(r.errors().empty());
    const auto parse = [&](std::string_view t) { return parsePath(t, r); };

    auto p = parse("scene[3].track[7].volume");
    REQUIRE(p);
    CHECK(p->scope == Scope::Track);
    CHECK(p->scene == 3);
    CHECK(p->track == 7);
    CHECK(p->param == idOf(r, "track.volume"));
    CHECK(engineScope(*p) == ((3 << 4) | 7));

    p = parse("scene[3].track[7].playback.pitch");
    REQUIRE(p);
    CHECK(p->param == idOf(r, "playback.pitch"));

    p = parse("scene[3].track[7].insertFx[1].chorus.rate");
    REQUIRE(p);
    CHECK(p->scope == Scope::InsertFx);
    CHECK(p->slot == 1);
    CHECK(p->param == idOf(r, "fx2.chorus.rate"));
    CHECK_FALSE(parse("scene[3].track[7].chorus.rate"));

    p = parse("scene[15].sendFx.reverb.size");
    REQUIRE(p);
    CHECK(p->scope == Scope::SendFx);
    CHECK(engineScope(*p) == 15);
    p = parse("scene[0].masterFx.compressor.ratio");
    REQUIRE(p);
    CHECK(p->scope == Scope::MasterFx);
    p = parse("scene[2].pattern[9].length");
    REQUIRE(p);
    CHECK(p->scope == Scope::Pattern);
    CHECK(p->pattern == 9);
    p = parse("scene[2].colour");
    REQUIRE(p);
    CHECK(p->param == idOf(r, "scene.colour"));
    p = parse("scene[2].routing.fxMode");
    REQUIRE(p);
    CHECK(p->scope == Scope::Scene);
    p = parse("scene[2].bpm"); // AFS-007 scene override
    REQUIRE(p);
    CHECK(p->param == idOf(r, "scene.bpm"));

    p = parse("settings.tempo");
    REQUIRE(p);
    CHECK(p->scope == Scope::Project);
    CHECK(p->param == idOf(r, "project.tempo"));
    CHECK(engineScope(*p) == 0);
    CHECK(parse("arrangement.play"));
    p = parse("preferences.settings.padCurve");
    REQUIRE(p);
    CHECK(p->param == idOf(r, "settings.padCurve"));

    // p-locks: lockable track and insert FX parameters only
    p = parse("scene[1].pattern[4].step[63].track[15].volume");
    REQUIRE(p);
    CHECK(p->scope == Scope::Step);
    CHECK(p->step == 63);
    CHECK(engineScope(*p) == ((1 << 4) | 15));
    p = parse("scene[1].pattern[4].step[0].track[0].insertFx[0].chorus.rate");
    REQUIRE(p);
    CHECK(p->param == idOf(r, "fx1.chorus.rate"));
    CHECK_FALSE(parse("scene[1].pattern[4].step[0].track[0].colour")); // not lockable
    CHECK(parse("scene[1].track[0].colour"));

    CHECK(formatPath({Scope::Track, 3, 7, 0, 0, 0, idOf(r, "track.volume")}, r) == "scene[3].track[7].volume");
    CHECK(formatPath({Scope::Project, 0, 0, 0, 0, 0, idOf(r, "project.tempo")}, r) == "settings.tempo");
    CHECK(formatPath({Scope::InsertFx, 0, 2, 0, 0, 0, idOf(r, "fx1.chorus.rate")}, r) ==
          "scene[0].track[2].insertFx[0].chorus.rate");
    CHECK(formatPath({Scope::Step, 1, 2, 3, 4, 1, idOf(r, "fx2.chorus.rate")}, r) ==
          "scene[1].pattern[3].step[4].track[2].insertFx[1].chorus.rate");
}

TEST_CASE("malformed or mismatched paths are rejected", "[model][path]")
{
    const auto& r = formsRegistry();
    for (std::string_view bad : {"", "scene[16].colour", "scene[3].track[16].volume", "scene[-1].colour",
                                 "scene[03].colour", "scene[3]colour", "scene[3].colour.", "scene[].colour",
                                 "scene[3].track[7].volume.x", "scene[3].track[7].insertFx[2].chorus.rate",
                                 "scene[3].track[7].insertFx[0].params[2]", "scene[3].pattern[1].step[64].track[0].volume",
                                 "scene[3].pattern[1].step[1].volume", "tempo", "project.tempo", "settings.padCurve",
                                 "preferences.padCurve", "track.volume", "scene[3].track[7].Volume",
                                 "scene[3 ].colour", "scene[1000].colour", "scene[3].sendFx.compressor.ratio"})
    {
        INFO(std::string(bad));
        CHECK_FALSE(parsePath(bad, r).has_value());
    }
    // a ParamPath whose scope does not fit its spec formats to ""
    const auto vol = idOf(r, "track.volume");
    CHECK(formatPath({Scope::Scene, 0, 0, 0, 0, 0, vol}, r).empty());
    CHECK(formatPath({Scope::InsertFx, 0, 0, 0, 0, 0, vol}, r).empty());
    CHECK(formatPath({Scope::InsertFx, 0, 0, 0, 0, 1, idOf(r, "fx1.chorus.rate")}, r).empty());
    CHECK(formatPath({Scope::Step, 0, 0, 0, 0, 0, idOf(r, "track.colour")}, r).empty()); // not lockable
    CHECK(formatPath({Scope::Track, 16, 0, 0, 0, 0, vol}, r).empty());
    CHECK(formatPath({Scope::Step, 0, 0, 0, 64, 0, vol}, r).empty());
    CHECK(formatPath({Scope::Track, 0, 0, 0, 0, 0, {}}, r).empty());
    CHECK(formatPath({Scope::Track, 0, 0, 0, 0, 0, core::ParamId {999}}, r).empty());
}

TEST_CASE("every path form round-trips and no two specs share a path", "[model][path]")
{
    const auto texts = roundTripAll(formsRegistry());
    CHECK(texts.size() > formsRegistry().size());
}

TEST_CASE("every registered ParamSpec has a path that round-trips", "[model][path]")
{
    const auto& r = core::builtinParams();
    REQUIRE(r.size() > 0);
    const auto texts = roundTripAll(r);
    CHECK(texts.size() >= r.size());
}
