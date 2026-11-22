// UndoStack (state-and-undo.md): undo/redo moves, the 256 MB cap with eviction (EDT-U1), the 1 s coalescing window
// (EDT-U2), checkpoints and the dirty state (EDT-007).
#include "CommandTestFields.h"
#include "core/commands/UndoStack.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
using namespace cmdtest;

// A runner like the bus's: inverse against the model, then apply.
struct Runner
{
    Project& p;
    std::unique_ptr<core::Command> operator()(core::Command& c) const
    {
        auto inv = c.inverse(p);
        c.apply(p);
        return inv;
    }
};

// Execute like the bus does (no group, coalescing at `nowMs`).
void exec(core::UndoStack& s, Project& p, std::unique_ptr<core::Command> c, double nowMs)
{
    auto inv = c->inverse(p);
    c->apply(p);
    const auto bytes = inv->sizeBytes();
    s.pushOrMerge(std::move(inv), bytes, nowMs);
}
} // namespace

TEST_CASE("undo stack: undo and redo move entries between the stacks", "[commands][undo]")
{
    auto p = std::make_unique<Project>();
    core::UndoStack s;
    CHECK_FALSE(s.canUndo());
    CHECK_FALSE(s.undo(Runner{*p}));
    exec(s, *p, setVolume(0, 0, 90), 0);
    exec(s, *p, setPan(0, 0, 10), 5000);
    CHECK(s.undoDepth() == 2);
    CHECK(s.undoLabel() == "PAN");
    REQUIRE(s.undo(Runner{*p}));
    CHECK(p->scenes[0].tracks[0].pan == 0);
    CHECK(s.canRedo());
    CHECK(s.redoLabel() == "PAN");
    REQUIRE(s.undo(Runner{*p}));
    CHECK(p->scenes[0].tracks[0].volume == 100);
    CHECK_FALSE(s.canUndo());
    REQUIRE(s.redo(Runner{*p}));
    REQUIRE(s.redo(Runner{*p}));
    CHECK(p->scenes[0].tracks[0].volume == 90);
    CHECK(p->scenes[0].tracks[0].pan == 10);
    CHECK_FALSE(s.canRedo());

    // A new edit clears the redo stack.
    s.undo(Runner{*p});
    exec(s, *p, setVolume(0, 1, 1), 9000);
    CHECK_FALSE(s.canRedo());
}

TEST_CASE("undo stack: same path within 1 s coalesces into one step (EDT-U2)", "[commands][undo]")
{
    auto p = std::make_unique<Project>();
    core::UndoStack s;
    // An encoder turn: 10 deltas 100 ms apart = one step, sliding window.
    for (int i = 1; i <= 10; ++i)
        exec(s, *p, setVolume(0, 3, 100 - i), 100.0 * i);
    CHECK(s.undoDepth() == 1);
    // More than 1 s later: a new step.
    exec(s, *p, setVolume(0, 3, 50), 100.0 * 10 + 1001);
    CHECK(s.undoDepth() == 2);
    // A different path never merges, and it breaks the run.
    exec(s, *p, setPan(0, 3, 5), 2200);
    exec(s, *p, setVolume(0, 3, 40), 2300);
    CHECK(s.undoDepth() == 4);
    // The same path again within 1 s merges into the step just started.
    exec(s, *p, setVolume(0, 3, 30), 2400);
    CHECK(s.undoDepth() == 4);

    s.undo(Runner{*p}); // 30, 40 → 5's state
    CHECK(p->scenes[0].tracks[3].volume == 50);
    s.undo(Runner{*p});
    s.undo(Runner{*p});
    CHECK(p->scenes[0].tracks[3].volume == 90);
    s.undo(Runner{*p});
    CHECK(p->scenes[0].tracks[3].volume == 100);
    // Redo of a coalesced step restores the final value of the turn.
    s.redo(Runner{*p});
    CHECK(p->scenes[0].tracks[3].volume == 90);
}

TEST_CASE("undo stack: undo, redo and checkpoints seal the top", "[commands][undo]")
{
    auto p = std::make_unique<Project>();
    core::UndoStack s;
    exec(s, *p, setVolume(0, 0, 90), 0);
    exec(s, *p, setVolume(0, 0, 80), 100);
    CHECK(s.undoDepth() == 1);
    s.checkpoint();
    exec(s, *p, setVolume(0, 0, 70), 200); // inside the window but past a save: own step
    CHECK(s.undoDepth() == 2);
    s.undo(Runner{*p});
    exec(s, *p, setVolume(0, 0, 60), 300); // after an undo: own step
    CHECK(s.undoDepth() == 2);
    s.undo(Runner{*p});
    CHECK(p->scenes[0].tracks[0].volume == 80);
}

TEST_CASE("undo stack: checkpoints and dirty state (EDT-007)", "[commands][undo]")
{
    auto p = std::make_unique<Project>();
    core::UndoStack s;
    CHECK_FALSE(s.dirty());
    exec(s, *p, setVolume(0, 0, 90), 0);
    CHECK(s.dirty());
    s.checkpoint(); // save
    CHECK_FALSE(s.dirty());
    exec(s, *p, setPan(0, 0, 3), 5000);
    CHECK(s.dirty());
    s.undo(Runner{*p});
    CHECK_FALSE(s.dirty()); // back at the saved state
    s.undo(Runner{*p});
    CHECK(s.dirty()); // undoing past the checkpoint marks it dirty again
    s.redo(Runner{*p});
    CHECK_FALSE(s.dirty());
    s.redo(Runner{*p});
    CHECK(s.dirty());
    // Branching away from the saved state: it is unreachable now, so dirty stays true whatever we undo.
    s.undo(Runner{*p});
    s.undo(Runner{*p});
    exec(s, *p, setTempo(99), 10000);
    s.undo(Runner{*p});
    CHECK(s.dirty());
    // clear() keeps the dirty state; a checkpoint after it makes it clean (project open).
    s.clear();
    CHECK(s.dirty());
    s.checkpoint();
    CHECK_FALSE(s.dirty());
    CHECK(s.bytes() == 0);
}

TEST_CASE("undo stack: the memory cap evicts the oldest history (EDT-U1)", "[commands][undo]")
{
    CHECK(core::UndoStack::kDefaultCapBytes == 256u * 1024u * 1024u);
    auto p = std::make_unique<Project>();
    const std::size_t one = setVolume(0, 0, 1)->inverse(*p)->sizeBytes();
    core::UndoStack s(one * 10);
    for (int i = 0; i < 25; ++i)
        exec(s, *p, setVolume(0, i % 10, i), 5000.0 * i); // same path length: same size
    CHECK(s.undoDepth() == 10);
    CHECK(s.bytes() <= s.capBytes());
    CHECK(s.evicted() == 15);
    // The ten newest edits are still undoable, in order.
    for (int i = 24; i >= 15; --i)
    {
        CHECK(p->scenes[0].tracks[i % 10].volume == i);
        REQUIRE(s.undo(Runner{*p}));
    }
    CHECK_FALSE(s.canUndo());
    CHECK(s.redoDepth() == 10);
    // Lowering the cap evicts at once; the newest entry is always kept.
    s.setCapBytes(1);
    CHECK(s.redoDepth() == 1);
    s.redo(Runner{*p});
    s.setCapBytes(1);
    CHECK(s.undoDepth() == 1);
    CHECK(s.redoDepth() == 0);
}
