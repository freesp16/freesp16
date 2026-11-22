// ParamSpec registry (UID-200/201): the single source of captions, ranges, steps and generated target lists.
#include "core/params/ParamRegistry.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("built-in catalogue holds the HOME strip parameters", "[params]")
{
    const auto& r = core::builtinParams();
    CHECK(r.errors().empty());
    for (auto id : {"track.volume", "track.pan", "playback.pitch", "playback.timeStretch", "playback.loop", "track.triggerMode"})
        CHECK(r.find(id) != nullptr);
    CHECK(r.find("track.volume")->label == "VOLUME");
    REQUIRE(r.byMidiCc(14).has_value());
    CHECK(r[*r.byMidiCc(14)].id == "playback.timeStretch");
    // The catalogue grows with every feature; the six HOME parameters are always lockable.
    const auto locks = r.lockable();
    for (auto id : {"track.volume", "track.pan", "playback.pitch", "playback.timeStretch", "playback.loop", "track.triggerMode"})
        CHECK(std::find(locks.begin(), locks.end(), *r.idOf(id)) != locks.end());
    CHECK(r.lfoTargets().size() >= 3);
}

TEST_CASE("formatting follows the spec wording", "[params]")
{
    const auto& r = core::builtinParams();
    char buf[32];
    auto fmt = [&](const char* id, float v) {
        const int n = r.find(id)->formatValue(v, buf, sizeof buf);
        return std::string(buf, static_cast<std::size_t>(n));
    };
    CHECK(fmt("track.volume", 100) == "100"); // requested: the track VOLUME reads 0–127 (law: 100 = 0.0 dB)
    CHECK(fmt("track.volume", 127) == "127");
    CHECK(fmt("track.volume", 0) == "0");
    CHECK(fmt("mixer.masterVolume", 100) == "100"); // every level reads 0–127, no dB (by request)
    CHECK(fmt("track.pan", 0) == "C");
    CHECK(fmt("track.pan", -64) == "L64");
    CHECK(fmt("track.pan", 12) == "R12");
    CHECK(fmt("playback.timeStretch", 2) == "MASTER TEMPO");
    CHECK(fmt("track.triggerMode", 0) == "ONE SHOT");
}

TEST_CASE("encoder steps clamp and stay integral", "[params]")
{
    const auto& vol = *core::builtinParams().find("track.volume");
    CHECK(vol.step(100, 5, false) == 105);
    CHECK(vol.step(125, 10, false) == 127);
    CHECK(vol.step(2, -10, false) == 0);
    const auto& pitch = *core::builtinParams().find("playback.pitch");
    CHECK(pitch.step(0, 3, true) == pitch.clamp(0.3f));
}

TEST_CASE("registry rejects bad descriptors", "[params]")
{
    core::ParamRegistry r;
    CHECK(r.add({.id = "a.b", .label = "AB"}).valid());
    CHECK_FALSE(r.add({.id = "a.b", .label = "AB again"}).valid()); // duplicate id
    CHECK_FALSE(r.add({.id = "a.c", .label = ""}).valid());          // no caption
    CHECK_FALSE(r.add({.id = "a.d", .label = "D", .type = core::ParamType::Enum, .minValue = 0, .maxValue = 2}).valid());
    CHECK(r.errors().size() == 3);
}
