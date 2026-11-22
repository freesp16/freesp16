// validate(): data-model.md invariants (SampleRef resolution) and the ranges / cross-references a decoder can break.
#include "core/model/ModelOps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <functional>
#include <limits>

using namespace core::model;
using Catch::Matchers::ContainsSubstring;

namespace
{
// Applies `mutate` to a fresh default project; expects exactly one problem line mentioning `expected`.
void expectProblem(const std::function<void(Project&)>& mutate, const std::string& expected, std::size_t count = 1)
{
    auto p = newDefaultProject();
    mutate(*p);
    std::vector<std::string> problems;
    CHECK_FALSE(validate(*p, problems));
    REQUIRE(problems.size() == count);
    CHECK_THAT(problems.front(), ContainsSubstring(expected));
}
} // namespace

TEST_CASE("validate: every track sample resolves to a pool entry", "[model]")
{
    auto p = newDefaultProject();
    p->scenes[3].tracks[7].playback.sample.id = "sha256:feed";
    std::vector<std::string> problems;
    CHECK_FALSE(validate(*p, problems));
    REQUIRE(problems.size() == 1);
    CHECK_THAT(problems[0], ContainsSubstring("scene[3].track[7].playback.sample"));

    problems.clear();
    p->sampleRefs.entries.push_back({"sha256:feed", "$Ext:USB/kick.wav", "KICK", true, {}}); // missing is a state
    CHECK(validate(*p, problems));
    CHECK(problems.empty());

    expectProblem(
        [](Project& q) {
            q.sampleRefs.entries.push_back({"a", "x", "", false, {}});
            q.sampleRefs.entries.push_back({"a", "y", "", false, {}});
        },
        "duplicates entry 0");
    expectProblem([](Project& q) { q.sampleRefs.entries.push_back({"", "x", "", false, {}}); }, "id: empty");
}

TEST_CASE("validate: value ranges and enum values", "[model]")
{
    expectProblem([](Project& q) { q.scenes[3].tracks[7].volume = 200; }, "scene[3].track[7].volume: 200 not in 0");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].pan = -65; }, "track[0].pan");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].chokeGroup = 9; }, "chokeGroup");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].mode = static_cast<TrackMode>(9); }, "is not a TrackMode");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].padScale.scaleType = 14; }, "is not a ScaleType");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].seq.retriggerCount = 0; }, "seq.retriggerCount");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].ampEnv.release = 1.5; }, "ampEnv.release");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].ampEnv.attack = std::nan(""); }, "ampEnv.attack");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].midi.notes[2] = 128; }, "midi.notes[2]");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].midi.length = 0.1; }, "midi.length");
    expectProblem([](Project& q) { q.settings.tempo = 10.0; }, "settings.tempo");
    expectProblem([](Project& q) { q.settings.tempo = std::numeric_limits<double>::infinity(); }, "settings.tempo");
    expectProblem([](Project& q) { q.settings.currentScene = 16; }, "settings.currentScene");
    expectProblem([](Project& q) { q.scenes[4].bpm = 0.0; }, "scene[4].bpm");
    expectProblem([](Project& q) { q.scenes[4].routing.headphoneSource = 13; }, "headphoneSource");
    expectProblem([](Project& q) { q.scenes[1].patterns[2].length = 65; }, "scene[1].pattern[2].length");
    expectProblem([](Project& q) { q.scenes[1].patterns[2].length = 0; }, "scene[1].pattern[2].length");
    expectProblem([](Project& q) { q.ribbon.user[1][7].top = 2.0; }, "ribbon.user[1][7].top");
    expectProblem([](Project& q) { q.midi.output.channels[3] = 17; }, "midi.output.channels[3]");
    expectProblem([](Project& q) { q.arrangement.sections.push_back({16, 0, 1, {}, {}}); }, "sections[0].scene");
    expectProblem([](Project& q) { q.arrangement.sections.push_back({0, 0, 0, {}, {}}); }, "sections[0].repeats");
    expectProblem([](Project& q) { q.arrangement.sections.resize(1000); }, "at most 999");

    // files may hold patterns of 1–64 steps (SEQ-001): a short one is fine
    auto p = newDefaultProject();
    p->scenes[0].patterns[0].length = 1;
    std::vector<std::string> problems;
    CHECK(validate(*p, problems));
}

TEST_CASE("validate: effect slots", "[model]")
{
    expectProblem([](Project& q) { q.scenes[0].tracks[1].insertFx[1].type = EffectType::Reverb; },
                  "track[1].insertFx[1].type: Reverb is not available on insert");
    expectProblem([](Project& q) { q.scenes[0].sendFx.type = EffectType::Ducker; }, "sendFx.type: Ducker");
    expectProblem([](Project& q) { q.scenes[0].masterFx.params[5] = 1.01; }, "masterFx.params[5]");
    expectProblem([](Project& q) { q.scenes[0].tracks[0].insertFx[0].lookAhead = true; }, "lookAhead");
    expectProblem(
        [](Project& q) {
            auto& slot = q.scenes[0].tracks[0].insertFx[0];
            slot.type = EffectType::Chorus;
            slot.typeMemory.push_back({EffectType::Chorus, 0, 0, {}, false, false});
        },
        "remembers the slot's current type");
    expectProblem(
        [](Project& q) {
            auto& slot = q.scenes[0].tracks[0].insertFx[0];
            slot.typeMemory.push_back({EffectType::Flanger, 0, 0, {}, false, false});
            slot.typeMemory.push_back({EffectType::Flanger, 0, 0, {}, false, false});
        },
        "remembered twice");

    auto p = newDefaultProject();
    auto& slot = p->scenes[0].tracks[0].insertFx[0];
    slot.type = EffectType::Chorus;
    slot.params = {0.0, 0.25, 0.5, 0.75, 1.0, 0.5};
    slot.typeMemory.push_back({EffectType::Flanger, 1, 0, {0.1, 0.2, 0.3, 0.4, 0.5, 0.6}, true, false});
    p->scenes[0].sendFx.type = EffectType::Compressor;
    p->scenes[0].sendFx.lookAhead = true;
    std::vector<std::string> problems;
    CHECK(validate(*p, problems));
}

TEST_CASE("validate: steps and sparse locks", "[model]")
{
    auto good = newDefaultProject();
    auto& pat = good->scenes[2].patterns[3];
    pat.steps[0][0].trigger = 0;
    pat.steps[0][0].value2 = 110;
    pat.steps[8][5].triggerType = TriggerType::LockOnly;
    pat.steps[8][5].trigger = 0;
    pat.steps[8][5].lockIndex = 0;
    StepLocks locks;
    locks.mask = (1ull << 0) | (1ull << 34);
    locks.values[0] = 300.0f;
    locks.values[34] = 0.42f;
    locks.change[34] = LockChange::Glide;
    pat.locks.push_back(locks);
    std::vector<std::string> problems;
    CHECK(validate(*good, problems));
    CHECK(problems.empty());

    const auto withLocks = [](Project& q) {
        auto& p = q.scenes[2].patterns[3];
        p.steps[8][5].trigger = 0;
        p.steps[8][5].lockIndex = 0;
        StepLocks l;
        l.mask = 1;
        p.locks.push_back(l);
    };
    expectProblem(
        [&](Project& q) {
            withLocks(q);
            q.scenes[2].patterns[3].steps[8][5].lockIndex = 1;
        },
        "scene[2].pattern[3].step[8].track[5].lockIndex: 1 but the pattern has 1", 2); // + the orphaned entry
    expectProblem(
        [&](Project& q) {
            withLocks(q);
            q.scenes[2].patterns[3].steps[9][5].lockIndex = 0;
        },
        "shared with another cell");
    expectProblem(
        [&](Project& q) {
            withLocks(q);
            q.scenes[2].patterns[3].locks.push_back(q.scenes[2].patterns[3].locks[0]);
        },
        "locks[1]: not referenced");
    expectProblem(
        [&](Project& q) {
            withLocks(q);
            q.scenes[2].patterns[3].locks[0].mask = 1ull << 35;
        },
        "slots beyond 34");
    expectProblem(
        [&](Project& q) {
            withLocks(q);
            q.scenes[2].patterns[3].locks[0].values[0] = std::numeric_limits<float>::quiet_NaN();
        },
        "values[0]: not finite");
    expectProblem([](Project& q) { q.scenes[0].patterns[0].steps[3][2].value2 = 0; }, "step[3].track[2].velocity");
    expectProblem([](Project& q) { q.scenes[0].patterns[0].steps[3][2].triggerType = static_cast<TriggerType>(7); },
                  "is not a TriggerType");
}

TEST_CASE("validate caps the number of problem lines", "[model]")
{
    auto p = newDefaultProject();
    for (auto& scene : p->scenes)
        for (auto& t : scene.tracks)
            t.volume = 500; // 256 problems
    std::vector<std::string> problems;
    CHECK_FALSE(validate(*p, problems, 10));
    REQUIRE(problems.size() == 11);
    CHECK(problems.back() == "246 more problems not listed");
}
