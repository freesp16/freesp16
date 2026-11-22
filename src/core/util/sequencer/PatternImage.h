// The model-neutral image of one pattern that the message thread hands to the engine's pattern builder (threading.md
// "Object swap protocol").
//
// Why it exists: the engine's immutable pattern object (engine/sequencer/PatternData.h) is built on the message thread
// from core::model::Pattern, but tools/check-deps.py lets no module include both: engine/** may not include
// core/model/Pattern.h, and ui / services / app may include only engine/EngineApi.h of the engine. So
// the message-thread half (core/commands/sequencer: makePatternImage) turns the model pattern into this image — core
// types only, already in the engine's units — and the engine registers its builder here at start-up
// (engine/sequencer/PatternBuilder.cpp, APP_STATIC_REGISTRAR). The caller then posts the built object
// as ObjectSwap{SwapSlot::Pattern, scene, pattern}, and destroys it itself when post() refuses it.
//
//   message thread:  PatternImage img; makePatternImage(model, img);  const void* obj = patternCompiler().build(img);
//                    if (! bridge.post(ObjectSwap{{SwapSlot::Pattern, scene, pattern}, obj})) patternCompiler().destroy(obj);
//
// No engine linked (core-only tests, tools): patternCompiler().build is null.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace core::sequencer
{
constexpr int kImageSteps = 64, kImageTracks = 16, kImageLockSlots = 35;
constexpr std::uint16_t kNoLocks = 0xFFFF;

// Lock slots the sequencer itself reads (sequencer.md "Lockable parameters", ParamIndex 15–17).
constexpr int kLockSlotOffset = 15, kLockSlotRetrigCount = 16, kLockSlotRetrigSpeed = 17;

struct ImageCell // one track at one step; velocity 0 = no trigger (SEQ-010)
{
    std::uint8_t velocity = 0;
    std::uint8_t kind = 0;        // 0 FULL, 1 HALF, 2 PARAM (= model::TriggerType = engine::trigger::TriggerKind)
    std::int8_t offset24 = 0;     // the step's OFF SET lock (slot 15), −24…+24; 0 without one
    std::uint8_t retrigCount = 0; // slot 16 lock, 0 = none (the track value applies)
    std::uint8_t retrigSpeed = 0; // slot 17 lock + 1, 0 = none
    std::uint16_t lockIndex = kNoLocks; // into PatternImage::locks
};

struct ImageLocks // the p-locks of one cell, native units (frames, cents, 0–127, enum index; SEQ-024)
{
    std::uint64_t mask = 0; // bit s = slot s locked
    std::array<float, kImageLockSlots> values {};
    std::array<std::uint8_t, kImageLockSlots> change {}; // 0 Revert, 1 Hold, 2 Glide (SEQ-021)
};

struct PatternImage
{
    std::uint8_t length = 16; // 1–64 (SEQ-001)
    std::uint16_t trackMutes = 0, trackSolos = 0;
    std::array<std::array<ImageCell, kImageTracks>, kImageSteps> cells; // [step][track]; no {} (GCC 11)
    std::vector<ImageLocks> locks;
};

// The engine's builder (message thread): allocates the engine's pattern object; null when no engine registered one.
using PatternBuildFn = const void* (*)(const PatternImage&);
using PatternDestroyFn = void (*)(const void*) noexcept;
struct PatternCompiler
{
    PatternBuildFn build = nullptr;
    PatternDestroyFn destroy = nullptr;
};
void setPatternCompiler(PatternCompiler) noexcept; // start-up registration only
PatternCompiler patternCompiler() noexcept;
} // namespace core::sequencer
