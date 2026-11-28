// EDT-004, EDT-U4: compare every stereo frame across a settings paste and its undo with an independent engine.
// The reference receives only the intended audible change. The edited engine receives the real clipboard command.
#include "tests/unit/edit/EditTestUtil.h"

#include "core/edit/SceneEditor.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "ui/input/pads/PadController.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/shell/ModelParamAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace ce = core::edit;

TEST_CASE("edit.engine: a track pasted while playing is heard from the next block, the transport keeps running",
          "[edit][engine]")
{
    auto ref = enginetest::make(true);
    auto e = enginetest::make(true);
    edittest::Bus b(false);
    auto& source = b.project().scenes[0].tracks[1];
    int destination = 0;
    SECTION("a paste mutes the sounding track")
    {
        source.mute = true;
    }
    SECTION("a paste preserves the sounding track's audio settings")
    {
        source.name = "Pasted settings"; // change the model while preserving the tone
    }
    SECTION("a paste changes another track while the tone plays")
    {
        source.mute = true;
        destination = 2;
    }
    const bool mutesTone = destination == 0 && source.mute;
    int hookCalls = 0;
    core::commands::setBridge(b.bus, [&](const core::ChangeSet& changes, const core::model::Project& p) {
        ++hookCalls;
        ui::input::pads::forwardTrackChanges(changes, p, e->bridge());
        ui::forwardModelChanges(changes, p, e->bridge());
        ui::fx::forwardFxChanges(changes, p, e->bridge());
    });
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor(b.bus, pool);
    for (auto* x : {ref.get(), e.get()})
        REQUIRE(x->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));

    constexpr int kPaste = engine::kBlockFrames * 300; // block-aligned: the paste lands between two blocks
    const auto compareAudio = [&](std::string_view phase, int frames)
    {
        CAPTURE(phase);
        const auto expected = enginetest::render(*ref, frames, engine::kBlockFrames);
        const auto actual = enginetest::render(*e, frames, engine::kBlockFrames);
        CHECK(std::ranges::equal(actual.l, expected.l));
        CHECK(std::ranges::equal(actual.r, expected.r));
        return actual;
    };
    const auto before = compareAudio("before paste", kPaste);
    REQUIRE(enginetest::peak(before.l) > 0.2f);
    REQUIRE(enginetest::peak(before.r) > 0.2f);

    editor.copy(ce::trackRef(0, 1, false));
    bool ok = false;
    editor.paste(ce::trackRef(0, destination, false), [&ok](ce::Result r) { ok = r.ok(); });
    REQUIRE(ok);
    REQUIRE(b.track(0, destination).name == source.name);
    REQUIRE(b.track(0, destination).mute == source.mute);
    CHECK(hookCalls == 1);
    const auto mute = core::builtinParams().idOf("track.mute").value();
    if (mutesTone)
        REQUIRE(ref->bridge().post(engine::ParamChange{mute, 0, 1.0f, 0})); // independent normal mute fade
    const auto after = compareAudio("after paste", 4096);
    if (mutesTone)
    {
        CHECK(enginetest::peak(after.l, 256) == 0.0f);
        CHECK(enginetest::peak(after.r, 256) == 0.0f);
    }
    else
    {
        CHECK(enginetest::peak(after.l) > 0.2f);
        CHECK(enginetest::peak(after.r) > 0.2f);
    }
    CHECK(e->bridge().playhead().playing);
    CHECK(e->bridge().playhead().ppq == ref->bridge().playhead().ppq);
#if defined(APP_CHECK_PLAYBACK_TIMING)
    // Release keeps the host's existing one-percent allowance. Sanitizer builds check the samples above.
    const auto load = e->load();
    CHECK(load.overruns <= 1 + load.blocks / 100);
#endif

    REQUIRE(b.bus.undo());
    if (mutesTone)
        REQUIRE(ref->bridge().post(engine::ParamChange{mute, 0, 0.0f, 0}));
    const auto restored = compareAudio("after undo", 4096);
    CHECK(enginetest::peak(restored.l, 512) > 0.2f);
    CHECK(enginetest::peak(restored.r, 512) > 0.2f);
}

#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/scenes/SceneSync.h"

#include <variant>

TEST_CASE("edit.engine: a scene paste re-posts only what differs: scene BPM, changed patterns, changed tracks",
          "[edit]")
{
    edittest::Bus b(false);
    auto& p = b.project();
    p.scenes[2].bpm = 133.0;
    p.scenes[2].patterns[4].length = 48;
    edittest::putStep(p.scenes[2].patterns[4], 3, 1, 100);
    p.scenes[2].tracks[6].volume = 12;
    fakes::FakeEngineBridge bridge;
    ui::input::scenes::SceneSync sync(core::builtinParams(), bridge);
    core::ChangeSet seen;
    core::commands::setBridge(b.bus, [&](const core::ChangeSet& c, const core::model::Project& model) {
        seen = c;
        sync.onChange(c, model);
        ui::forwardModelChanges(c, model, bridge);
    });
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor(b.bus, pool);
    editor.copy(ce::sceneRef(2));
    editor.paste(ce::sceneRef(5)); // scene 6 is a default scene: only pattern 5, track 7 and the scene fields differ
    CHECK(seen.touches("scene[5].pattern[4]"));
    CHECK_FALSE(seen.touches("scene[5].pattern[3]"));
    CHECK(seen.touches("scene[5].track[6].volume"));
    CHECK_FALSE(seen.touches("scene[5].track[5]"));
    auto posted = [&bridge](const char* id, std::uint16_t scope) {
        const auto pid = core::builtinParams().idOf(id).value();
        float v = -1.0f;
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == pid && c->scope == scope)
                v = c->value;
        return v;
    };
    CHECK(posted("scene.bpm", 5) == 133.0f);
    CHECK(posted("pattern.length", 5 << 4 | 4) == 48.0f);
    CHECK(posted("track.volume", 5 << 4 | 6) == 12.0f);
}
