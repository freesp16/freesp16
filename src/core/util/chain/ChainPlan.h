// The pattern chain the engine plays (spec 02-features/scenes-and-patterns.md SCN-030/031): an immutable
// plain value built on the message thread (core/edit/PatternChainBuilder.h) and installed with
// ObjectSwap{SwapSlot::ProjectConfig, kSwapScene, kSwapIndex}; engine/scene/PatternChain.h plays it (step 5).
//
// Why core/util: tools/check-deps.py lets the engine include core/util but not core/edit, and lets ui / app
// include only engine/EngineApi.h of the engine, so the one type both halves share lives here (like the sequencer's
// PatternImage). The object is a plain `new ChainPlan`, freed by destroyChainPlan on the message thread (RetireQueue).
//
//   message thread:  auto* plan = new ChainPlan(builder.end().value());
//                    if (! bridge.post(ObjectSwap{{SwapSlot::ProjectConfig, kSwapScene, kSwapIndex}, plan}))
//                        destroyChainPlan(plan);
//   length 0 = no chain (posting it clears the chain); a chain loops until a new plan arrives or the player sees a
//   switch it did not ask for (a manual selection ends it, SCN-030 "loops until changed").
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace core::chain
{
inline constexpr int kMaxChain = 64; // entries, repeats allowed (1→2→2→4)
// The swap slot of the chain plan: SwapSlot::ProjectConfig, scene 0, index 0 (the slot kind has no other user).
inline constexpr std::uint8_t kSwapScene = 0, kSwapIndex = 0;

struct ChainPlan
{
    std::uint8_t scene = 0;  // 0-based scene the chain belongs to (it plays only while that scene is current)
    std::uint8_t length = 0; // 0…64 entries; 0 = no chain
    std::array<std::uint8_t, kMaxChain> patterns {}; // 0-based patterns, in play order
};
static_assert(std::is_trivially_copyable_v<ChainPlan>, "the chain plan is a plain value");

// SwapTable DestroyFn of the slot (message thread only: the retire queue calls it).
inline void destroyChainPlan(const void* plan) noexcept
{
    delete static_cast<const ChainPlan*>(plan); // rt-ok: message thread only (the retire queue frees retired plans)
}
} // namespace core::chain
