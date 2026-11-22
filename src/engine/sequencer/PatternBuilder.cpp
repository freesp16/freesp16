// Message thread only (allocates): the `rt-ok` lines are the builder's own storage.
#include "engine/sequencer/PatternBuilder.h"

#include "core/util/Registrar.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace engine::sequencer
{
namespace
{
namespace cs = ::core::sequencer;

// The owner: PatternData first (what the slot points at), then the storage its LockSets point into.
struct PatternObject final : PatternData
{
    std::vector<trigger::LockSet> sets;  // rt-ok: message-thread builder storage
    std::vector<float> values;           // rt-ok: message-thread builder storage
    std::vector<std::uint8_t> change;    // rt-ok: message-thread builder storage
};

const void* buildErased(const cs::PatternImage& image) { return buildPattern(image); }
} // namespace

const PatternData* buildPattern(const cs::PatternImage& image)
{
    auto* p = new PatternObject; // rt-ok: message-thread builder (the swap's object)
    PatternData& d = *p;
    d.length = static_cast<std::uint8_t>(std::clamp<int>(image.length, 1, 64));
    d.trackMutes = image.trackMutes;
    d.trackSolos = image.trackSolos;
    std::memset(d.triggerMask, 0, sizeof d.triggerMask);

    const std::size_t numLocks = image.locks.size();
    p->sets.resize(numLocks);                              // rt-ok: message-thread builder
    p->values.resize(numLocks * cs::kImageLockSlots);      // rt-ok: message-thread builder
    p->change.resize(numLocks * cs::kImageLockSlots);      // rt-ok: message-thread builder
    for (std::size_t i = 0; i < numLocks; ++i)
    {
        const auto& src = image.locks[i];
        float* v = p->values.data() + i * cs::kImageLockSlots;
        std::uint8_t* c = p->change.data() + i * cs::kImageLockSlots;
        std::copy(src.values.begin(), src.values.end(), v);
        std::copy(src.change.begin(), src.change.end(), c);
        p->sets[i] = trigger::LockSet {src.mask & ((std::uint64_t {1} << cs::kImageLockSlots) - 1), v, c};
    }
    d.locks = std::span<const trigger::LockSet>(p->sets.data(), p->sets.size());

    for (int s = 0; s < 64; ++s)
        for (int t = 0; t < 16; ++t)
        {
            const auto& in = image.cells[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            StepCell& c = d.cells[s][t];
            c.velocity = in.velocity > 127 ? 127 : in.velocity;
            c.kind = in.kind == 1 ? trigger::TriggerKind::Half
                     : in.kind == 2 ? trigger::TriggerKind::LockOnly
                                    : trigger::TriggerKind::Full;
            c.offset24 = static_cast<std::int8_t>(std::clamp<int>(in.offset24, -24, 24));
            c.retrigCount = in.retrigCount;
            c.retrigSpeed = in.retrigSpeed;
            c.lockIndex = in.lockIndex < numLocks ? in.lockIndex : kNoLockIndex;
            if (c.velocity != 0)
                d.triggerMask[t] |= std::uint64_t {1} << s;
        }
    return p;
}

void destroyPattern(const void* object) noexcept
{
    delete static_cast<const PatternObject*>(static_cast<const PatternData*>(object)); // rt-ok: message thread (retire)
}
} // namespace engine::sequencer

APP_STATIC_REGISTRAR(::core::sequencer::setPatternCompiler({&::engine::sequencer::buildErased,
                                                            &::engine::sequencer::destroyPattern}))
