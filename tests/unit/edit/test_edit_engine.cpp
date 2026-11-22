// A paste during playback (EDT-004, EDT-U4): the transport keeps running, nothing overruns, and the pasted values are
// heard from the next engine block — checked by an offline render against a reference engine without the paste. The
// bridge hook is the composition root's (app/Document.cpp): the track, table and FX forwarders read the edit's
// expanded change paths.
#include "tests/unit/edit/EditTestUtil.h"

#include "core/edit/SceneEditor.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "ui/input/pads/PadController.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/shell/ModelParamAccess.h"

#include <catch2/catch_test_macros.hpp>

namespace ce = core::edit;

TEST_CASE("edit.engine: a track pasted while playing is heard from the next block, the transport keeps running",
          "[edit][engine]")
{
    auto ref = enginetest::make(true);
    auto e = enginetest::make(true);
    edittest::Bus b(false);
    b.project().scenes[0].tracks[1].mute = true; // the source: a muted track 2
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

    constexpr int kPaste = 64 * 300; // block-aligned: the paste lands between two blocks
    auto a = enginetest::render(*ref, kPaste, 64);
    auto c = enginetest::render(*e, kPaste, 64);
    REQUIRE(a.l == c.l);
    editor.copy(ce::trackRef(0, 1, false));
    bool ok = false;
    editor.paste(ce::trackRef(0, 0, false), [&ok](ce::Result r) { ok = r.ok(); }); // track 1 = the test tone
    REQUIRE(ok);
    CHECK(hookCalls == 1); // one command, one bridge call
    const auto a2 = enginetest::render(*ref, 4096, 64);
    const auto c2 = enginetest::render(*e, 4096, 64);
    std::size_t firstDiff = a2.l.size();
    for (std::size_t i = 0; i < a2.l.size(); ++i)
        if (a2.l[i] != c2.l[i])
        {
            firstDiff = i;
            break;
        }
    CHECK(firstDiff <= 1);                         // from the first frame of the next block (mute fade, MIX-002)
    CHECK(enginetest::peak(c2.l, 256) == 0.0f);    // silent after the fade
    CHECK(enginetest::peak(a2.l, 256) > 0.2f);     // the reference keeps playing
    CHECK(e->bridge().playhead().playing);         // the transport never stopped
    CHECK(e->bridge().playhead().ppq > ref->bridge().playhead().ppq - 1e-9);
    // Wall-clock overruns on a loaded host without an RT kernel (ASan, parallel ctest): ≤ 1 % of the blocks, the
    // margin tests/unit/perf/test_load_meter.cpp uses; the device budget is checked on hardware.
    CHECK(e->load().overruns <= 1 + e->load().blocks / 100);
    // Undo during playback is audible at the next block too (EDT-U4).
    REQUIRE(b.bus.undo());
    const auto c3 = enginetest::render(*e, 4096, 64);
    CHECK(enginetest::peak(c3.l, 512) > 0.2f);
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
