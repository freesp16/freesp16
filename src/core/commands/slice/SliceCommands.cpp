#include "core/commands/slice/SliceCommands.h"

#include "core/util/playback/Grid.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace core::commands::slice
{
namespace
{
using core::slice::kAddedTag;
using core::slice::SlicePoints;

class SetSlices final : public Command
{
public:
    SetSlices(int scene, int track, model::SliceSettings value, std::string_view label, std::string key)
        : scene_(scene & 15), track_(track & 15), value_(std::move(value)), label_(label), key_(std::move(key))
    {
    }
    std::string_view label() const override { return label_; }
    void apply(model::Project& p) override
    {
        p.scenes[static_cast<std::size_t>(scene_)].tracks[static_cast<std::size_t>(track_)].slices = value_;
    }
    std::unique_ptr<Command> inverse(const model::Project& before) const override
    {
        return std::make_unique<SetSlices>(scene_, track_, settings(before, scene_, track_), label_, key_);
    }
    bool mergeWith(const Command& other, double) override
    {
        const auto* o = dynamic_cast<const SetSlices*>(&other);
        return o != nullptr && ! key_.empty() && o->key_ == key_ && o->scene_ == scene_ && o->track_ == track_;
    }
    void collectChanges(ChangeSet& c) const override { c.add(slicesPath(scene_, track_)); }
    std::size_t sizeBytes() const override { return sizeof(*this) + value_.grids.capacity() * sizeof(std::int64_t); }

private:
    int scene_, track_;
    model::SliceSettings value_;
    std::string_view label_;
    std::string key_;
};

std::unique_ptr<Command> make(const model::Project& p, int scene, int track, model::SliceSettings s,
                              std::string_view label, std::string key)
{
    if (s == settings(p, scene, track))
        return nullptr;
    return std::make_unique<SetSlices>(scene, track, std::move(s), label, std::move(key));
}

// The stored pair index of point `pt` (a moved grid point or an added point), −1 when it is not stored.
int storedPair(const model::SliceSettings& s, const core::slice::SlicePoint& pt) noexcept
{
    for (std::size_t i = 0; i + 1 < s.grids.size(); i += 2)
    {
        if (pt.grid >= 0 && s.grids[i] == pt.grid)
            return static_cast<int>(i / 2);
        if (pt.grid < 0 && s.grids[i] == kAddedTag && s.grids[i + 1] == pt.frame)
            return static_cast<int>(i / 2);
    }
    return -1;
}

void erasePair(model::SliceSettings& s, int pair)
{
    const auto at = s.grids.begin() + 2 * pair;
    s.grids.erase(at, at + 2);
}

} // namespace

std::string slicesPath(int scene, int track)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "scene[%d].track[%d].slices", scene & 15, track & 15);
    return buf;
}

bool parseSlicesPath(std::string_view path, int& scene, int& track) noexcept
{
    constexpr std::string_view a = "scene[", b = "].track[", c = "].slices";
    if (path.substr(0, a.size()) != a)
        return false;
    path.remove_prefix(a.size());
    const auto num = [&path](int& out) {
        const auto r = std::from_chars(path.data(), path.data() + path.size(), out);
        if (r.ec != std::errc {} || out < 0 || out > 15)
            return false;
        path.remove_prefix(static_cast<std::size_t>(r.ptr - path.data()));
        return true;
    };
    if (! num(scene) || path.substr(0, b.size()) != b)
        return false;
    path.remove_prefix(b.size());
    if (! num(track) || path.substr(0, c.size()) != c)
        return false;
    path.remove_prefix(c.size());
    return path.empty();
}

const model::SliceSettings& settings(const model::Project& p, int scene, int track) noexcept
{
    return p.scenes[static_cast<std::size_t>(scene & 15)].tracks[static_cast<std::size_t>(track & 15)].slices;
}

std::unique_ptr<Command> setCount(const model::Project& p, int scene, int track, int count)
{
    model::SliceSettings s = settings(p, scene, track);
    s.gridCount = core::slice::validCount(count);
    s.grids.clear();
    s.windowOffset = core::slice::clampArea(s.windowOffset, s.gridCount);
    return make(p, scene, track, std::move(s), "GRID SLICE", std::string());
}

std::unique_ptr<Command> movePoint(const model::Project& p, int scene, int track, int slice, std::int64_t frame,
                                   std::int64_t frames)
{
    const model::SliceSettings& cur = settings(p, scene, track);
    const SlicePoints pts = core::slice::slicePoints(cur, frames);
    if (slice < 0 || slice >= pts.count)
        return nullptr;
    const std::int64_t lo = slice > 0 ? pts.start(slice - 1) + 1 : 0;
    const std::int64_t hi = pts.end(slice) - 1;
    if (hi < lo)
        return nullptr;
    frame = std::clamp(frame, lo, hi);
    const auto& pt = pts.point[static_cast<std::size_t>(slice)];
    if (frame == pt.frame)
        return nullptr;
    model::SliceSettings s = cur;
    if (const int pair = storedPair(s, pt); pair >= 0)
        erasePair(s, pair);
    if (pt.grid >= 0)
    {
        if (frame != core::slice::gridFrame(pt.grid, core::slice::gridCountOf(s), frames)) // back on the grid = not moved
            s.grids.insert(s.grids.end(), {static_cast<std::int64_t>(pt.grid), frame});
    }
    else
        s.grids.insert(s.grids.end(), {kAddedTag, frame});
    return make(p, scene, track, std::move(s), "POSITION", "move" + std::to_string(slice));
}

std::unique_ptr<Command> nudgePoint(const model::Project& p, int scene, int track, int slice, int detents, bool fine,
                                    std::int64_t frames)
{
    const SlicePoints pts = core::slice::slicePoints(settings(p, scene, track), frames);
    if (slice < 0 || slice >= pts.count || detents == 0)
        return nullptr;
    const std::int64_t step = fine ? 1 : core::playback::coarseStep(frames);
    return movePoint(p, scene, track, slice, pts.start(slice) + step * detents, frames);
}

bool canAdd(const model::SliceSettings& s, std::int64_t frames) noexcept
{
    const SlicePoints pts = core::slice::slicePoints(s, frames);
    return frames > 1 && pts.count < core::slice::kMaxSlices;
}

bool canDelete(const model::SliceSettings& s, int slice, std::int64_t frames) noexcept
{
    const SlicePoints pts = core::slice::slicePoints(s, frames);
    return slice > 0 && slice < pts.count && pts.point[static_cast<std::size_t>(slice)].grid < 0;
}

std::int64_t addFrame(const model::SliceSettings& s, int slice, std::int64_t playhead, std::int64_t frames) noexcept
{
    const SlicePoints pts = core::slice::slicePoints(s, frames);
    if (pts.count == 0)
        return 0;
    slice = std::clamp(slice, 0, pts.count - 1);
    const std::int64_t a = pts.start(slice), b = pts.end(slice);
    if (playhead > a && playhead < b)
        return playhead;
    return a + (b - a) / 2;
}

std::unique_ptr<Command> addPoint(const model::Project& p, int scene, int track, std::int64_t frame, std::int64_t frames)
{
    const model::SliceSettings& cur = settings(p, scene, track);
    if (! canAdd(cur, frames) || frame <= 0 || frame >= frames)
        return nullptr;
    const SlicePoints pts = core::slice::slicePoints(cur, frames);
    for (int i = 0; i < pts.count; ++i)
        if (pts.start(i) == frame)
            return nullptr; // already a point: a slice is never empty
    model::SliceSettings s = cur;
    s.grids.insert(s.grids.end(), {kAddedTag, frame});
    return make(p, scene, track, std::move(s), "ADD SLICE", std::string());
}

std::unique_ptr<Command> deletePoint(const model::Project& p, int scene, int track, int slice, std::int64_t frames)
{
    const model::SliceSettings& cur = settings(p, scene, track);
    if (! canDelete(cur, slice, frames))
        return nullptr;
    const SlicePoints pts = core::slice::slicePoints(cur, frames);
    model::SliceSettings s = cur;
    const int pair = storedPair(s, pts.point[static_cast<std::size_t>(slice)]);
    if (pair < 0)
        return nullptr;
    erasePair(s, pair);
    s.windowOffset = core::slice::clampArea(s.windowOffset, pts.count - 1);
    return make(p, scene, track, std::move(s), "DELETE SLICE", std::string());
}

std::unique_ptr<Command> setArea(const model::Project& p, int scene, int track, int area, std::int64_t frames)
{
    model::SliceSettings s = settings(p, scene, track);
    const int count = frames > 0 ? core::slice::slicePoints(s, frames).count : core::slice::gridCountOf(s);
    s.windowOffset = core::slice::clampArea(area, count);
    return make(p, scene, track, std::move(s), "AREA", "area");
}

std::unique_ptr<Command> replaceSlices(int scene, int track, model::SliceSettings s, std::string_view label)
{
    return std::make_unique<SetSlices>(scene, track, std::move(s), label, std::string());
}
} // namespace core::commands::slice
