// Building a pattern chain on the panel (spec 02-features/scenes-and-patterns.md SCN-030): hold the current pattern's
// key in PATTERN mode and press others → the chain [current, pressed…], up to 64 entries, repeats allowed (1→2→2→4).
// Releasing the held key ends the gesture; the chain plan goes to the engine as an immutable object
// (core/util/chain/ChainPlan.h shows the post), which loops it until it changes. Message thread only.
//
//   ui (PATTERN mode):  press(current)  → begin(scene, current)          the plain select is deferred
//                       press(n)        → add(n)                          true = consumed (no select)
//                       release(current)→ end(): plan → post it;  nullopt → it was a plain press of the current
//                                         pattern (cancel, SCN-021): do the normal select
//   a manual selection while a chain plays: post clear() (the engine also ends a chain on a switch it did not ask for)
#pragma once

#include "core/util/chain/ChainPlan.h"

#include <memory>
#include <optional>

namespace core::edit
{
using core::chain::ChainPlan;

class PatternChainBuilder
{
public:
    void begin(int scene, int currentPattern) noexcept;
    bool add(int pattern) noexcept;              // false when not holding or the chain is full (64)
    std::optional<ChainPlan> end() noexcept;     // a plan when ≥ 1 pattern was added
    void cancel() noexcept { holding_ = false; } // the mode was left while holding
    bool holding() const noexcept { return holding_; }
    int size() const noexcept { return plan_.length; }

    static ChainPlan clear() noexcept { return ChainPlan {}; } // length 0 = no chain
    // The engine object (message thread): post it as ObjectSwap{SwapSlot::ProjectConfig, kSwapScene, kSwapIndex};
    // on refusal free it with core::chain::destroyChainPlan.
    static std::unique_ptr<ChainPlan> object(const ChainPlan& p) { return std::make_unique<ChainPlan>(p); }

private:
    ChainPlan plan_ {};
    bool holding_ = false;
};
} // namespace core::edit
