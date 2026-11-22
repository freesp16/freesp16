// Pastes through the step sequencer (PatternSync on the bus's bridge hook, the app's path): a pattern and
// steps pasted while the transport plays are sequenced from their next pass, and the chain 1→2→2→4 plays the pasted
// patterns with every step at its exact frame (offline, block by block).
#include "tests/unit/edit/EditTestUtil.h"
#include "tests/unit/sequencer/SeqRig.h"

#include "core/edit/SceneEditor.h"
#include "engine/scene/PatternChain.h"
#include "ui/input/scenes/SceneSync.h"
#include "ui/input/steps/PatternSync.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace ce = core::edit;

TEST_CASE("edit.sequencer: patterns pasted while playing are sequenced and chained at the exact frames", "[edit][engine]")
{
    seqtest::Rig rig;
    edittest::Bus b(false);
    auto& p1 = b.project().scenes[0].patterns[0];
    seqtest::put(p1, 0, 0);
    seqtest::put(p1, 4, 0);
    seqtest::put(p1, 9, 0);
    ui::input::scenes::SceneSync sceneSync(core::builtinParams(), rig.e->bridge());
    core::commands::setBridge(b.bus, [&](const core::ChangeSet& c, const core::model::Project& p) {
        ui::input::steps::forwardPatternChanges(c, p, rig.e->bridge());
        sceneSync.onChange(c, p);
    });
    ui::input::steps::forwardAllSequencer(b.project(), rig.e->bridge());
    rig.blocks(4);
    auto* plan = new engine::scene::ChainPlan {};
    plan->length = 4;
    plan->patterns = {0, 1, 1, 3};
    REQUIRE(rig.e->bridge().post(engine::ObjectSwap {engine::scene::kChainSlot, plan}));
    REQUIRE(rig.play());
    rig.until(30000); // inside pattern 1's first pass (88 200 frames)
    ce::InlineWorkerPool pool;
    ce::SceneEditor editor(b.bus, pool);
    editor.copy(ce::patternRef(0, 0));
    editor.paste(ce::patternRef(0, 1)); // pattern 2 = pattern 1
    editor.copy(ce::stepsRef(0, 0, 0, 1ull << 0 | 1ull << 4));
    editor.paste(ce::stepsRef(0, 3, 0, 1ull << 2)); // pattern 4: steps 3 and 7
    rig.until(5 * 88200 + 100);

    const double step = seqtest::stepFrames();
    std::vector<long> want;
    const int passes[] = {0, 1, 1, 3, 0};
    for (int pass = 0; pass < 5; ++pass)
    {
        const double start = 16 * step * pass; // 16 steps a pass
        const std::vector<int> steps = passes[pass] == 3 ? std::vector<int> {2, 6} : std::vector<int> {0, 4, 9};
        for (const int s : steps)
            want.push_back(std::lround(start + s * step)); // from the first step's frame
    }
    std::vector<long> got;
    for (const auto& h : rig.sequenced(0))
        got.push_back(h.frame);
    INFO("hits " << got.size());
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i)
        CHECK(std::labs((got[i] - got[0]) - want[i]) <= 1);
    rig.e->bridge().retirePending();
}
