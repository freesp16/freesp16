#include "core/commands/slice/SlicePoints.h"

namespace core::slice
{
int validCount(int count) noexcept
{
    if (count <= 0)
        return kDefaultCount;
    int best = kGridCounts[0];
    for (const int c : kGridCounts)
    {
        const int d = c > count ? c - count : count - c, db = best > count ? best - count : count - best;
        if (d < db)
            best = c;
    }
    return best;
}

int gridCountOf(const model::SliceSettings& s) noexcept
{
    return s.gridCount >= 1 && s.gridCount <= kMaxSlices ? s.gridCount : validCount(s.gridCount);
}

SlicePoints slicePoints(const model::SliceSettings& s, std::int64_t frames) noexcept
{
    SlicePoints out;
    out.frames = frames > 0 ? frames : 0;
    if (frames <= 0)
        return out;
    const int n = gridCountOf(s);

    // Candidates: the grid with its moved points, then the added points (each pair of `grids` is one edit).
    std::array<SlicePoint, 2 * kMaxSlices> cand {};
    int m = 0;
    for (int g = 0; g < n; ++g)
        cand[static_cast<std::size_t>(m++)] = {gridFrame(g, n, frames), static_cast<std::int8_t>(g), false};
    const std::size_t pairs = s.grids.size() / 2;
    for (std::size_t i = 0; i < pairs; ++i)
    {
        const std::int64_t tag = s.grids[2 * i], frame = s.grids[2 * i + 1];
        if (tag >= 0 && tag < n)
        {
            auto& p = cand[static_cast<std::size_t>(tag)];
            p.frame = frame;
            p.moved = frame != gridFrame(static_cast<int>(tag), n, frames);
        }
        else if (tag == kAddedTag && m < static_cast<int>(cand.size()))
            cand[static_cast<std::size_t>(m++)] = {frame, -1, false};
    }
    // Stable insertion sort by frame (grid points before added points on a tie).
    for (int i = 1; i < m; ++i)
    {
        const SlicePoint v = cand[static_cast<std::size_t>(i)];
        int j = i;
        while (j > 0 && cand[static_cast<std::size_t>(j - 1)].frame > v.frame)
        {
            cand[static_cast<std::size_t>(j)] = cand[static_cast<std::size_t>(j - 1)];
            --j;
        }
        cand[static_cast<std::size_t>(j)] = v;
    }
    for (int i = 0; i < m && out.count < kMaxSlices; ++i)
    {
        const SlicePoint& p = cand[static_cast<std::size_t>(i)];
        if (p.frame < 0 || p.frame >= frames)
            continue;
        if (out.count > 0 && p.frame <= out.point[static_cast<std::size_t>(out.count - 1)].frame)
            continue; // a slice is never empty
        out.point[static_cast<std::size_t>(out.count++)] = p;
    }
    return out;
}

int areaCount(int slices) noexcept { return slices <= kPadsPerArea ? 1 : (slices + kPadsPerArea - 1) / kPadsPerArea; }

int clampArea(int area, int slices) noexcept
{
    const int last = areaCount(slices) - 1;
    return area < 0 ? 0 : (area > last ? last : area);
}

int sliceForPad(int pad, int area, int slices) noexcept
{
    if (pad < 0 || pad >= kPadsPerArea)
        return -1;
    const int s = clampArea(area, slices) * kPadsPerArea + pad;
    return s < slices ? s : -1;
}

SliceTable buildTable(const model::SliceSettings& s, std::int64_t frames) noexcept
{
    const SlicePoints pts = slicePoints(s, frames);
    SliceTable t;
    t.slices = pts.count;
    t.window = clampArea(s.windowOffset, pts.count);
    t.frames = pts.frames;
    for (int i = 0; i < pts.count; ++i)
        t.points[static_cast<std::size_t>(i)] = pts.start(i);
    t.points[static_cast<std::size_t>(pts.count)] = pts.frames;
    return t;
}

int editedPoints(const model::SliceSettings& s) noexcept { return static_cast<int>(s.grids.size() / 2); }
} // namespace core::slice
