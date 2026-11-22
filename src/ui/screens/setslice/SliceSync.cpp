#include "ui/screens/setslice/SliceSync.h"

#include "core/commands/slice/SliceCommands.h"
#include "core/commands/slice/SlicePoints.h"
#include "core/util/slice/SliceTable.h"

#include <string>

namespace ui::slice
{
bool postTable(engine::EngineBridge& bridge, const core::model::Project& p, int scene, int track, std::int64_t frames)
{
    const engine::SwapSlot slot {engine::SwapSlot::SliceMap, static_cast<std::uint8_t>(scene & 15),
                                 static_cast<std::uint8_t>(track & 15)};
    if (frames <= 0)
        return bridge.post(engine::ObjectSwap {slot, nullptr}); // no sample: the engine's default view
    auto* table = new core::slice::SliceTable(
        core::slice::buildTable(core::commands::slice::settings(p, scene, track), frames));
    if (bridge.post(engine::ObjectSwap {slot, table}))
        return true;
    core::slice::destroySliceTable(table);
    return false;
}

void postAll(engine::EngineBridge& bridge, const core::model::Project& p, const FramesFn& frames)
{
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
        {
            const std::int64_t n = frames ? frames(s, t) : 0;
            if (n > 0) // empty slots stay empty (nothing installed yet)
                postTable(bridge, p, s, t, n);
        }
}

void SliceSync::onChange(const core::ChangeSet& changes, const core::model::Project& p)
{
    for (const auto& path : changes.paths())
    {
        int s = 0, t = 0;
        if (core::commands::slice::parseSlicesPath(path, s, t))
            postTable(bridge_, p, s, t, frames(s, t));
    }
}

void SliceSync::onInstalled(int scene, int track, std::int64_t frames, const core::model::Project& p)
{
    postTable(bridge_, p, scene, track, frames);
}

void SliceLive::onHit(int track, int slice, double nowMs, bool recording) noexcept
{
    if (track < 0 || track > 15 || slice < 0 || slice > 63)
        return;
    lastSlice[static_cast<std::size_t>(track)] = slice;
    lastHitMs[static_cast<std::size_t>(track)] = nowMs;
    sliceHitMs[static_cast<std::size_t>(slice)] = nowMs;
    lastTrack = track;
    if (! recording)
        return;
    if (numPending == kPending) // the oldest goes (it was never recorded)
    {
        for (int i = 1; i < kPending; ++i)
            pending[static_cast<std::size_t>(i - 1)] = pending[static_cast<std::size_t>(i)];
        --numPending;
    }
    pending[static_cast<std::size_t>(numPending++)] = {static_cast<std::int8_t>(track), static_cast<std::int8_t>(slice), nowMs};
}

std::optional<int> SliceLive::takeRecorded(int track, double nowMs) noexcept
{
    int kept = 0;
    std::optional<int> out;
    for (int i = 0; i < numPending; ++i)
    {
        const Pending e = pending[static_cast<std::size_t>(i)];
        if (nowMs - e.ms > kPairMs)
            continue; // stale: no recording took it
        if (! out && e.track == track)
        {
            out = e.slice;
            continue;
        }
        pending[static_cast<std::size_t>(kept++)] = e;
    }
    numPending = kept;
    return out;
}

SliceLive& sliceLive()
{
    static SliceLive live;
    return live;
}

int sliceCount(const core::model::Project& p, int scene, int track, std::int64_t frames) noexcept
{
    const auto& s = core::commands::slice::settings(p, scene, track);
    if (frames > 0)
        return core::slice::slicePoints(s, frames).count;
    int n = core::slice::gridCountOf(s);
    for (std::size_t i = 0; i + 1 < s.grids.size(); i += 2)
        if (s.grids[i] == core::slice::kAddedTag)
            ++n;
    return n > core::slice::kMaxSlices ? core::slice::kMaxSlices : n;
}

int padSlice(const core::model::Project& p, int scene, int track, int pad, std::int64_t frames) noexcept
{
    const int count = frames > 0 ? sliceCount(p, scene, track, frames) : 16;
    const int area = frames > 0 ? core::commands::slice::settings(p, scene, track).windowOffset : 0;
    return core::slice::sliceForPad(pad, area, count);
}
} // namespace ui::slice
