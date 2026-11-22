// The ParamRegistry is the only source of captions, formats, lock slots, LFO destinations, strip targets, the MIDI CC
// map and file keys (UID-200/201). Every consumer list is derived from the registry and agrees with
// the ParamSpec flags; every format produces text for its range; scopes and storage keys are well-formed.
#include "core/params/ParamRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

TEST_CASE("generated consumer lists agree with the ParamSpec flags", "[contracts][params]")
{
    const auto& r = core::builtinParams();
    REQUIRE(r.errors().empty());
    REQUIRE(r.size() >= 6);
    std::size_t lockable = 0, lfo = 0, strip = 0;
    for (std::uint16_t i = 0; i < r.size(); ++i)
    {
        const auto& s = r[core::ParamId {i}];
        INFO(s.id);
        lockable += s.lockable;
        lfo += s.lfoTarget;
        strip += s.stripTarget;
        CHECK(r.idOf(s.id)->value == i); // ids are dense and stable
        if (s.midiCc >= 0)
            CHECK(r.byMidiCc(s.midiCc)->value == i);
        if (s.scope == core::ParamScope::Global)
        {
            CHECK_FALSE(s.lockable); // preferences are never p-locked or modulated
            CHECK_FALSE(s.lfoTarget);
        }
    }
    CHECK(r.lockable().size() == lockable);
    CHECK(r.lfoTargets().size() == lfo);
    CHECK(r.stripTargets().size() == strip);
    for (auto id : r.lfoTargets())
        CHECK(r[id].lfoTarget);
    for (auto id : r.lockable())
        CHECK(r[id].lockable);
    std::size_t byScope = 0;
    for (auto scope : {core::ParamScope::Track, core::ParamScope::Scene, core::ParamScope::Project,
                       core::ParamScope::SceneOverrideElseProject, core::ParamScope::Global})
        byScope += r.byScope(scope).size();
    CHECK(byScope == r.size());
}

TEST_CASE("every format produces text across the range; storage keys are unique", "[contracts][params]")
{
    const auto& r = core::builtinParams();
    std::set<std::string_view> keys;
    for (const auto& s : r.all())
    {
        INFO(s.id);
        char buf[48];
        for (float v : {s.minValue, s.defaultValue, s.maxValue})
        {
            const int n = s.formatValue(v, buf, sizeof buf);
            CHECK(n > 0);
            CHECK(n < static_cast<int>(sizeof buf));
        }
        CHECK(s.step(s.maxValue, 1, false) == s.maxValue);
        CHECK(s.step(s.minValue, -1, true) == s.minValue);
        CHECK(keys.insert(s.storageKey()).second);
        CHECK_FALSE(s.shortLabel.empty());
        CHECK(s.shortLabel.size() <= 8);
    }
}

TEST_CASE("HOME strip parameters keep their catalogue areas and scopes", "[contracts][params]")
{
    const auto& r = core::builtinParams();
    for (auto id : {"track.volume", "track.pan", "track.triggerMode", "playback.pitch", "playback.timeStretch", "playback.loop"})
    {
        INFO(id);
        REQUIRE(r.find(id) != nullptr);
        CHECK(r.find(id)->scope == core::ParamScope::Track);
    }
    const auto catalogues = core::paramCatalogues();
    REQUIRE(catalogues.size() >= 2);
    for (std::size_t i = 1; i < catalogues.size(); ++i)
        CHECK(catalogues[i - 1].order <= catalogues[i].order);
}
