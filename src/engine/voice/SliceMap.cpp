#include "engine/voice/SliceMap.h"

#include "engine/Engine.h"

namespace engine::voice
{
void acceptSliceSwaps(Engine& engine) noexcept { engine.acceptSwaps(SwapSlot::SliceMap, &core::slice::destroySliceTable); }

bool sliceRegion(const SliceMap* map, std::int64_t frames, int slice, SliceRegion& out) noexcept
{
    if (frames <= 0 || slice < 0)
        return false;
    if (map != nullptr && map->frames == frames)
    {
        if (slice >= map->count())
            return false;
        out = map->region(slice);
        return true;
    }
    constexpr int kDefault = 16; // SLC-001: an unsliced sample plays as 16 equal slices
    if (slice >= kDefault)
        return false;
    const std::int64_t a = static_cast<std::int64_t>(slice) * frames / kDefault;
    const std::int64_t b = static_cast<std::int64_t>(slice + 1) * frames / kDefault;
    if (b <= a)
        return false;
    out = {a, b};
    return true;
}
} // namespace engine::voice
