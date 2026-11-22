// Every edit kind round-trips: copy → paste gives the source content at the
// destination and nothing else changes; undo restores the original model (hash of the saved form and value equality);
// redo restores the pasted state; each edit is one undo step (EDT-001…003, EDT-007, EDT-U1).
#include "tests/unit/edit/EditTestUtil.h"

#include "core/commands/UndoStack.h"
#include "core/edit/SceneEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ce = core::edit;
namespace m = core::model;
using edittest::cell;

namespace
{
struct Rig
{
    edittest::Bus b;
    core::edit::InlineWorkerPool pool;
    ce::SceneEditor editor {b.bus, pool};
};

// paste src → dst, check `content` on the pasted model, then undo = original and redo = pasted.
template <typename Check>
void roundTrip(Rig& r, const ce::EditRef& src, const ce::EditRef& dst, Check&& content)
{
    const m::Project before = r.b.project();
    const auto h0 = edittest::hash(before);
    const auto depth = r.b.undoDepth();
    r.editor.copy(src);
    REQUIRE(r.editor.canPaste(dst));
    bool ok = false;
    r.editor.paste(dst, [&ok](ce::Result res) { ok = res.ok(); });
    REQUIRE(ok);
    CHECK(r.b.undoDepth() == depth + 1); // one undo step
    content(before, r.b.project());
    const m::Project pasted = r.b.project();
    const auto h1 = edittest::hash(pasted);
    REQUIRE(r.b.bus.undo());
    CHECK(edittest::hash(r.b.project()) == h0);
    CHECK(r.b.project() == before);
    REQUIRE(r.b.bus.redo());
    CHECK(edittest::hash(r.b.project()) == h1);
    CHECK(r.b.project() == pasted);
    REQUIRE(r.b.bus.undo());
    CHECK(r.b.project() == before);
}

void sameExcept(const m::Project& a, const m::Project& b, int scene)
{
    for (int s = 0; s < m::kScenes; ++s)
        if (s != scene)
            CHECK(a.scenes[static_cast<std::size_t>(s)] == b.scenes[static_cast<std::size_t>(s)]);
    CHECK(a.settings == b.settings);
}
} // namespace

TEST_CASE("edit.roundtrip: scene copy → paste → undo = original", "[edit]")
{
    Rig r;
    roundTrip(r, ce::sceneRef(1), ce::sceneRef(6), [](const m::Project& before, const m::Project& after) {
        CHECK(after.scenes[6] == before.scenes[1]);
        sameExcept(before, after, 6);
    });
}

TEST_CASE("edit.roundtrip: pattern copy → paste (across scenes, length and masks included) → undo", "[edit]")
{
    Rig r;
    roundTrip(r, ce::patternRef(0, 3), ce::patternRef(2, 9), [](const m::Project& before, const m::Project& after) {
        CHECK(after.scenes[2].patterns[9] == before.scenes[0].patterns[3]);
        for (int p = 0; p < m::kPatterns; ++p)
            if (p != 9)
                CHECK(after.scenes[2].patterns[static_cast<std::size_t>(p)] ==
                      before.scenes[2].patterns[static_cast<std::size_t>(p)]);
        CHECK(after.scenes[2].tracks == before.scenes[2].tracks);
        sameExcept(before, after, 2);
    });
}

TEST_CASE("edit.roundtrip: track (settings only) copy → paste → undo, the sequences stay", "[edit]")
{
    Rig r;
    roundTrip(r, ce::trackRef(0, 2, false), ce::trackRef(3, 11, false),
              [](const m::Project& before, const m::Project& after) {
                  CHECK(after.scenes[3].tracks[11] == before.scenes[0].tracks[2]);
                  CHECK(after.scenes[3].patterns == before.scenes[3].patterns);
                  sameExcept(before, after, 3);
              });
}

TEST_CASE("edit.roundtrip: track with sequences copies its column of all 16 patterns, locks included", "[edit]")
{
    Rig r;
    roundTrip(r, ce::trackRef(1, 4, true), ce::trackRef(1, 9, false), [](const m::Project& before, const m::Project& after) {
        const auto& src = before.scenes[1];
        const auto& dst = after.scenes[1];
        CHECK(dst.tracks[9] == src.tracks[4]);
        for (int p = 0; p < m::kPatterns; ++p)
        {
            const auto& sp = src.patterns[static_cast<std::size_t>(p)];
            const auto& dp = dst.patterns[static_cast<std::size_t>(p)];
            CHECK(dp.trackMutes[9] == sp.trackMutes[4]);
            CHECK(dp.trackSolos[9] == sp.trackSolos[4]);
            CHECK(dp.length == sp.length); // the destination keeps its own length (same scene here)
            for (int st = 0; st < m::kSteps; ++st)
            {
                CHECK(cell(dp, st, 9) == cell(sp, st, 4));
                for (int t = 0; t < m::kTracks; ++t)
                    if (t != 9)
                        CHECK(cell(dp, st, t) == cell(sp, st, t));
            }
        }
    });
}

TEST_CASE("edit.roundtrip: sequence copy → paste keeps the step positions in a shorter pattern (EDT-006)", "[edit]")
{
    Rig r;
    r.b.project().scenes[2].patterns[5].length = 16;
    r.b.project().scenes[0].patterns[0].length = 64;
    roundTrip(r, ce::sequenceRef(0, 0, 3), ce::sequenceRef(2, 5, 7), [](const m::Project& before, const m::Project& after) {
        const auto& sp = before.scenes[0].patterns[0];
        const auto& dp = after.scenes[2].patterns[5];
        CHECK(dp.length == 16);
        for (int st = 0; st < m::kSteps; ++st) // steps 17…64 kept in the matrix, not played
            CHECK(cell(dp, st, 7) == cell(sp, st, 3));
        for (int st = 0; st < m::kSteps; ++st)
            for (int t = 0; t < m::kTracks; ++t)
                if (t != 7)
                    CHECK(cell(dp, st, t) == cell(before.scenes[2].patterns[5], st, t));
    });
}

TEST_CASE("edit.roundtrip: steps and page copy → paste → undo", "[edit]")
{
    Rig r;
    SECTION("page of all tracks onto another page")
    {
        r.b.project().scenes[0].patterns[1].length = 64;
        roundTrip(r, ce::pageRef(0, 0, ce::kAllTracks, 0), ce::pageRef(0, 1, ce::kAllTracks, 2),
                  [](const m::Project& before, const m::Project& after) {
                      const auto& sp = before.scenes[0].patterns[0];
                      const auto& dp = after.scenes[0].patterns[1];
                      for (int st = 0; st < 16; ++st)
                          for (int t = 0; t < m::kTracks; ++t)
                              CHECK(cell(dp, 32 + st, t) == cell(sp, st, t));
                      for (int st = 0; st < 32; ++st)
                          for (int t = 0; t < m::kTracks; ++t)
                              CHECK(cell(dp, st, t) == cell(before.scenes[0].patterns[1], st, t));
                  });
    }
    SECTION("three steps of one track onto another track")
    {
        r.b.project().scenes[1].patterns[2].length = 32;
        const std::uint64_t mask = (1ull << 4) | (1ull << 5) | (1ull << 9);
        roundTrip(r, ce::stepsRef(1, 2, 6, mask), ce::stepsRef(1, 2, 0, 1ull << 20),
                  [](const m::Project& before, const m::Project& after) {
                      const auto& sp = before.scenes[1].patterns[2];
                      const auto& dp = after.scenes[1].patterns[2];
                      CHECK(cell(dp, 20, 0) == cell(sp, 4, 6));
                      CHECK(cell(dp, 21, 0) == cell(sp, 5, 6));
                      CHECK(cell(dp, 25, 0) == cell(sp, 9, 6));
                      CHECK(cell(dp, 22, 0) == cell(sp, 22, 0)); // not in the mask: untouched
                      CHECK(cell(dp, 4, 6) == cell(sp, 4, 6));   // the source stays
                  });
    }
}

TEST_CASE("edit.roundtrip: reset of every kind is one undo step and undoes exactly (EDT-003, EDT-012)", "[edit]")
{
    Rig r;
    const ce::EditRef refs[] = {ce::sceneRef(1),          ce::patternRef(0, 2),          ce::trackRef(2, 5, false),
                                ce::trackRef(2, 5, true), ce::sequenceRef(3, 1, 8),      ce::pageRef(0, 4, 3, 1),
                                ce::stepsRef(1, 1, ce::kAllTracks, 0xF0F0ull)};
    for (const auto& ref : refs)
    {
        INFO("kind " << static_cast<int>(ref.kind));
        const m::Project before = r.b.project();
        const auto depth = r.b.undoDepth();
        r.editor.reset(ref);
        CHECK(r.b.undoDepth() == depth + 1);
        CHECK_FALSE(r.b.project() == before);
        REQUIRE(r.b.bus.undo());
        CHECK(r.b.project() == before);
        REQUIRE(r.b.bus.redo());
        REQUIRE(r.b.bus.undo());
    }
    // The defaults themselves.
    r.editor.reset(ce::sceneRef(1));
    m::Scene fresh = r.b.project().scenes[0];
    m::resetScene(fresh, 1);
    CHECK(r.b.project().scenes[1] == fresh);
    r.editor.reset(ce::patternRef(0, 2));
    CHECK(r.b.pattern(0, 2) == m::defaultPattern());
    r.editor.reset(ce::trackRef(2, 5, true));
    CHECK(r.b.track(2, 5) == m::defaultTrack(5));
    for (int p = 0; p < m::kPatterns; ++p)
        for (int st = 0; st < m::kSteps; ++st)
            CHECK(r.b.pattern(2, p).steps[static_cast<std::size_t>(st)][5].empty());
}

TEST_CASE("edit.roundtrip: swap two tracks with and without sequences (EDT-002) is exact and its own inverse", "[edit]")
{
    Rig r;
    const m::Project before = r.b.project();
    r.editor.swapTracks(0, 1, 14, false);
    CHECK(r.b.track(0, 1) == before.scenes[0].tracks[14]);
    CHECK(r.b.track(0, 14) == before.scenes[0].tracks[1]);
    CHECK(r.b.project().scenes[0].patterns == before.scenes[0].patterns);
    REQUIRE(r.b.bus.undo());
    CHECK(r.b.project() == before);

    r.editor.swapTracks(0, 1, 14, true);
    for (int p = 0; p < m::kPatterns; ++p)
        for (int st = 0; st < m::kSteps; ++st)
        {
            CHECK(cell(r.b.pattern(0, p), st, 1) == cell(before.scenes[0].patterns[static_cast<std::size_t>(p)], st, 14));
            CHECK(cell(r.b.pattern(0, p), st, 14) == cell(before.scenes[0].patterns[static_cast<std::size_t>(p)], st, 1));
        }
    CHECK(r.b.undoDepth() == 1);
    REQUIRE(r.b.bus.undo());
    CHECK(r.b.project() == before);
    REQUIRE(r.b.bus.redo());
    REQUIRE(r.b.bus.undo());
    CHECK(edittest::hash(r.b.project()) == edittest::hash(before));
}

TEST_CASE("edit.roundtrip: generic swap of scenes, patterns, tracks across scenes and sequences", "[edit]")
{
    Rig r;
    const m::Project before = r.b.project();
    const std::pair<ce::EditRef, ce::EditRef> pairs[] = {
        {ce::sceneRef(0), ce::sceneRef(3)},
        {ce::patternRef(0, 1), ce::patternRef(2, 7)},
        {ce::trackRef(0, 3, true), ce::trackRef(1, 8, true)},
        {ce::sequenceRef(0, 0, 2), ce::sequenceRef(0, 0, 9)},
        {ce::sequenceRef(0, 4, 2), ce::sequenceRef(3, 6, 0)},
    };
    for (const auto& [a, b] : pairs)
    {
        INFO("kind " << static_cast<int>(a.kind));
        REQUIRE(r.editor.swap(a, b));
        CHECK_FALSE(r.b.project() == before);
        REQUIRE(r.b.bus.undo());
        CHECK(r.b.project() == before);
    }
    REQUIRE(r.editor.swap(ce::sceneRef(0), ce::sceneRef(3)));
    CHECK(r.b.project().scenes[0] == before.scenes[3]);
    CHECK(r.b.project().scenes[3] == before.scenes[0]);
    CHECK_FALSE(r.editor.swap(ce::sceneRef(0), ce::patternRef(0, 0))); // families must match
    CHECK_FALSE(r.editor.swap(ce::pageRef(0, 0, 0, 0), ce::pageRef(0, 0, 0, 1)));
}

TEST_CASE("edit.roundtrip: the undo cap charges the inverse's deep size (EDT-U1)", "[edit]")
{
    Rig r;
    r.editor.copy(ce::sceneRef(1));
    r.editor.paste(ce::sceneRef(2));
    const auto& h = core::commands::history(r.b.bus);
    CHECK(h.bytes() >= sizeof(m::Scene));
    CHECK(h.bytes() >= ce::deepBytes(r.b.project().scenes[1]) / 2);
}
