// Document model headers (data-model.md; the headers fix the shape, the model code fills defaults and validation):
// every entity default-constructs, the fixed-capacity invariants hold by construction, every entity carries an extras
// bag, a step cell stays 8 bytes so a project stays ≈ 2 MB.
#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/model/paths/ParamPath.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

TEST_CASE("a default project has the spec's fixed capacities", "[contracts][model]")
{
    auto p = std::make_unique<core::model::Project>();
    CHECK(p->scenes.size() == 16);
    for (const auto& scene : p->scenes)
    {
        CHECK(scene.tracks.size() == 16);
        CHECK(scene.patterns.size() == 16);
        CHECK_FALSE(scene.bpm.has_value()); // follows the project tempo
        for (const auto& pattern : scene.patterns)
        {
            CHECK(pattern.steps.size() == 64);
            CHECK(pattern.steps[0].size() == 16);
            CHECK(pattern.steps[63][15].empty());
            CHECK(pattern.locks.empty());
        }
    }
    CHECK(p->scenes[3].tracks[7].volume == 100);
    CHECK(p->scenes[3].tracks[7].insertFx.size() == 2);
    CHECK(p->settings.tempo == 120.0);
    CHECK(p->arrangement.sections.empty());
    STATIC_REQUIRE(sizeof(core::model::Step) == 8);
    CHECK(sizeof(core::model::Project) < 4u * 1024u * 1024u);
    UNSCOPED_INFO("sizeof(Project) = " << sizeof(core::model::Project));

    auto copy = std::make_unique<core::model::Project>(*p);
    CHECK(*copy == *p);
    copy->scenes[15].patterns[15].steps[63][15].trigger = 1;
    CHECK_FALSE(*copy == *p);
}

TEST_CASE("extras keep opaque round-trip data per entity", "[contracts][model]")
{
    core::model::Track t;
    CHECK(t.extras.empty());
    t.extras.set("sp16.unknownEnum", "WOBBLE", core::model::Extras::Encoding::Text);
    t.extras.set("sp16.unknownEnum", "WARBLE", core::model::Extras::Encoding::Text);
    REQUIRE(t.extras.find("sp16.unknownEnum") != nullptr);
    CHECK(t.extras.find("sp16.unknownEnum")->value == "WARBLE");
    CHECK(t.extras.entries.size() == 1);
    core::model::Section s;
    s.extras.set("sp16.record", "AAECAwQFBgc=", core::model::Extras::Encoding::Base64); // OQ-02 raw record
    CHECK(s.extras.find("sp16.record")->encoding == core::model::Extras::Encoding::Base64);
    // every entity has one (compile-time check of the extras rule)
    core::model::Project p0;
    (void) p0.extras, (void) p0.settings.extras, (void) p0.scenes[0].extras, (void) p0.scenes[0].routing.extras,
        (void) p0.scenes[0].sendFx.extras, (void) p0.scenes[0].patterns[0].extras, (void) t.playback.extras,
        (void) t.ampEnv.extras, (void) t.seq.extras, (void) t.slices.extras, (void) t.padScale.extras,
        (void) t.midiScale.extras, (void) t.send.extras, (void) t.lfo.extras, (void) t.midi.extras,
        (void) t.routing.extras, (void) p0.arrangement.extras, (void) p0.ribbon.extras, (void) p0.midi.extras,
        (void) p0.sampleRefs.extras;
}

TEST_CASE("paths map to engine scopes", "[contracts][model]")
{
    core::model::ParamPath track {core::model::Scope::Track, 3, 7, 0, 0, 0, {}};
    CHECK(core::model::engineScope(track) == ((3 << 4) | 7));
    core::model::ParamPath scene {core::model::Scope::SendFx, 5, 0, 0, 0, 0, {}};
    CHECK(core::model::engineScope(scene) == 5);
    core::model::ParamPath project {core::model::Scope::Project, 0, 0, 0, 0, 0, {}};
    CHECK(core::model::engineScope(project) == 0);
    CHECK_FALSE(project.param.valid());
}

namespace
{
// A command shaped like the bus's SetParam: the contract compiles and behaves as documented.
class SetTempo final : public core::Command
{
public:
    explicit SetTempo(double bpm) : bpm_(bpm) {}
    std::string_view label() const override { return "BPM"; }
    void apply(core::model::Project& p) override { p.settings.tempo = bpm_; }
    std::unique_ptr<core::Command> inverse(const core::model::Project& before) const override
    {
        return std::make_unique<SetTempo>(before.settings.tempo);
    }
    void collectChanges(core::ChangeSet& c) const override { c.add("settings.tempo"); }

private:
    double bpm_;
};
} // namespace

TEST_CASE("Command contract: apply, inverse, changes", "[contracts][commands]")
{
    auto p = std::make_unique<core::model::Project>();
    SetTempo cmd(131.0);
    auto undo = cmd.inverse(*p);
    cmd.apply(*p);
    CHECK(p->settings.tempo == 131.0);
    undo->apply(*p);
    CHECK(p->settings.tempo == 120.0);
    CHECK(cmd.undoable());
    CHECK_FALSE(cmd.mergeWith(*undo, 0.0));
    core::ChangeSet changes;
    cmd.collectChanges(changes);
    CHECK(changes.touches("settings"));
    CHECK(changes.touches(""));
    CHECK_FALSE(changes.touches("scene[0]"));

    int cancelled = 0;
    {
        core::Subscription s([&] { ++cancelled; });
        core::Subscription moved = std::move(s);
        CHECK_FALSE(s.active());
        CHECK(moved.active());
    }
    CHECK(cancelled == 1);
}
