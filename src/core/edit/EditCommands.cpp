#include "core/edit/EditCommands.h"

#include "core/model/ModelOps.h"

#include <utility>

namespace core::edit
{
namespace
{
using model::Pattern;
using model::Project;
using model::Scene;
using model::Track;

constexpr std::uint64_t kAllSteps = ~std::uint64_t {0};

constexpr std::size_t at(int i) noexcept
{
    return static_cast<std::size_t>(i & 15);
}

std::uint16_t trackBit(int track) noexcept
{
    return static_cast<std::uint16_t>(1u << (track & 15));
}

std::uint16_t trackMaskOf(const EditRef& r) noexcept
{
    return r.track > 15 ? std::uint16_t {0xFFFF} : trackBit(r.track);
}

const Pattern& patternOf(const Project& p, int scene, int pattern) noexcept
{
    return p.scenes[at(scene)].patterns[at(pattern)];
}

// A value prepared on the worker, handed to the (copyable) build function and moved into the holder once.
template <typename T>
using Box = std::shared_ptr<std::unique_ptr<T>>;
template <typename T>
Box<T> box(std::unique_ptr<T> v)
{
    return std::make_shared<std::unique_ptr<T>>(std::move(v));
}

// Exchanges two slots of one kind in the model (Scene / Pattern / Track); its own inverse, no copies.
class SwapSlotsCommand final : public Command
{
public:
    SwapSlotsCommand(const EditRef& a, const EditRef& b) noexcept : a_(a), b_(b) {}
    std::string_view label() const override
    {
        switch (familyOf(a_.kind))
        {
            case Family::Scene: return "SWAP SCENE";
            case Family::Pattern: return "SWAP PATTERN";
            default: return "SWAP TRACK";
        }
    }
    void apply(Project& p) override
    {
        switch (familyOf(a_.kind))
        {
            case Family::Scene:
                if (a_.scene != b_.scene)
                    std::swap(p.scenes[at(a_.scene)], p.scenes[at(b_.scene)]);
                break;
            case Family::Pattern:
                if (a_.scene != b_.scene || a_.pattern != b_.pattern)
                    std::swap(p.scenes[at(a_.scene)].patterns[at(a_.pattern)],
                              p.scenes[at(b_.scene)].patterns[at(b_.pattern)]);
                break;
            default:
                if (a_.scene != b_.scene || a_.track != b_.track)
                    std::swap(p.scenes[at(a_.scene)].tracks[at(a_.track)], p.scenes[at(b_.scene)].tracks[at(b_.track)]);
                break;
        }
    }
    std::unique_ptr<Command> inverse(const Project&) const override
    {
        return std::make_unique<SwapSlotsCommand>(a_, b_);
    }
    void collectChanges(ChangeSet& c) const override
    {
        for (const EditRef* r : {&a_, &b_})
            switch (familyOf(r->kind))
            {
                case Family::Scene: addScenePaths(c, r->scene); break;
                case Family::Pattern: addPatternPath(c, r->scene, r->pattern); break;
                default: addTrackPaths(c, r->scene, r->track); break;
            }
    }

private:
    EditRef a_, b_;
};

// The new pattern (scene, pattern) = the model's with `edit` applied.
template <typename Edit>
void editPattern(const Project& model, Subtrees& next, int scene, int pattern, Edit&& edit)
{
    auto np = std::make_unique<Pattern>(patternOf(model, scene, pattern));
    edit(*np);
    next.addPattern(scene, pattern, std::move(np));
}
} // namespace

std::shared_ptr<const ClipPayload> snapshot(const Project& project, const EditRef& src)
{
    auto clip = std::make_shared<ClipPayload>();
    clip->source = src;
    clip->project = &project;
    const auto& scene = project.scenes[at(src.scene)];
    switch (src.kind)
    {
        case ObjectKind::Scene: clip->scene = std::make_unique<Scene>(scene); break;
        case ObjectKind::Pattern: clip->pattern = std::make_unique<Pattern>(scene.patterns[at(src.pattern)]); break;
        case ObjectKind::Track: clip->track = std::make_unique<Track>(scene.tracks[at(src.track)]); break;
        case ObjectKind::TrackWithSequences:
            clip->track = std::make_unique<Track>(scene.tracks[at(src.track)]);
            clip->cells.reserve(model::kPatterns);
            for (const auto& p : scene.patterns)
                clip->cells.push_back(extractCells(p, trackBit(src.track), kAllSteps, true));
            break;
        case ObjectKind::Sequence:
            clip->cells.push_back(extractCells(scene.patterns[at(src.pattern)], trackBit(src.track), kAllSteps, true));
            break;
        case ObjectKind::Steps:
        case ObjectKind::Page:
            if (src.stepMask == 0)
                return nullptr;
            clip->cells.push_back(extractCells(scene.patterns[at(src.pattern)], trackMaskOf(src), src.stepMask, false));
            break;
    }
    return clip;
}

bool canPaste(const ClipPayload& clip, const EditRef& dst, const void* dstProject) noexcept
{
    const Family f = familyOf(clip.source.kind);
    if (f != familyOf(dst.kind))
        return false;
    if ((f == Family::Track || f == Family::Scene) && clip.project != dstProject)
        return false; // sample references belong to the source project (EDT-008 is P2)
    return true;
}

std::unique_ptr<Command> preparePaste(std::shared_ptr<const ClipPayload> clip, const EditRef& dst)
{
    if (! clip || familyOf(clip->source.kind) != familyOf(dst.kind))
        return nullptr;
    const int s = dst.scene & 15;
    switch (clip->source.kind)
    {
        case ObjectKind::Scene:
        {
            auto v = box(std::make_unique<Scene>(*clip->scene)); // the worker's deep copy (≈ 2 MB worst case)
            return std::make_unique<EditCommand>("PASTE SCENE", [v, s](const Project&, Subtrees& next) {
                next.addScene(s, std::move(*v));
            });
        }
        case ObjectKind::Pattern:
        {
            auto v = box(std::make_unique<Pattern>(*clip->pattern));
            const int p = dst.pattern & 15;
            return std::make_unique<EditCommand>("PASTE PATTERN", [v, s, p](const Project&, Subtrees& next) {
                next.addPattern(s, p, std::move(*v));
            });
        }
        case ObjectKind::Track:
        case ObjectKind::TrackWithSequences:
        {
            auto v = box(std::make_unique<Track>(*clip->track));
            const int t = dst.track & 15;
            const bool withSequences = clip->source.kind == ObjectKind::TrackWithSequences;
            return std::make_unique<EditCommand>(
                "PASTE TRACK", [v, s, t, withSequences, clip](const Project& model, Subtrees& next) {
                    next.addTrack(s, t, std::move(*v));
                    if (withSequences)
                        for (int p = 0; p < model::kPatterns; ++p)
                            editPattern(model, next, s, p, [&](Pattern& np) {
                                writeCells(np, clip->cells[static_cast<std::size_t>(p)], nullptr, t);
                            });
                });
        }
        case ObjectKind::Sequence:
        {
            const int p = dst.pattern & 15, t = dst.track & 15;
            return std::make_unique<EditCommand>("PASTE SEQUENCE", [clip, s, p, t](const Project& model, Subtrees& next) {
                editPattern(model, next, s, p, [&](Pattern& np) { writeCells(np, clip->cells.front(), nullptr, t); });
            });
        }
        case ObjectKind::Steps:
        case ObjectKind::Page:
        {
            const int p = dst.pattern & 15;
            const int first = lowestStep(dst.stepMask);
            // One track onto another (the destination's), all tracks onto the same tracks.
            const int trackTo = clip->source.track > 15 || dst.track > 15 ? -1 : (dst.track & 15);
            return std::make_unique<EditCommand>(
                "PASTE STEPS", [clip, s, p, first, trackTo](const Project& model, Subtrees& next) {
                    editPattern(model, next, s, p, [&](Pattern& np) {
                        const auto& cells = clip->cells.front();
                        const int length = np.length;
                        writeCells(np, cells,
                                   [&](int step) { return placeStep(step, cells.stepMask, first, length); }, trackTo);
                    });
                });
        }
    }
    return nullptr;
}

std::unique_ptr<Command> makeReset(const EditRef& r)
{
    const int s = r.scene & 15, p = r.pattern & 15, t = r.track & 15;
    switch (r.kind)
    {
        case ObjectKind::Scene:
            return std::make_unique<EditCommand>("DELETE SCENE", [s](const Project& model, Subtrees& next) {
                // A copy reset in place: no default-constructed Scene temporary (GCC 11).
                auto v = std::make_unique<Scene>(model.scenes[at(s)]);
                model::resetScene(*v, s);
                next.addScene(s, std::move(v));
            });
        case ObjectKind::Pattern:
            return std::make_unique<EditCommand>("DELETE PATTERN", [s, p](const Project&, Subtrees& next) {
                next.addPattern(s, p, std::make_unique<Pattern>(model::defaultPattern()));
            });
        case ObjectKind::Track:
        case ObjectKind::TrackWithSequences:
        {
            const bool withSequences = r.kind == ObjectKind::TrackWithSequences;
            return std::make_unique<EditCommand>("RESET TRACK", [s, t, withSequences](const Project& model, Subtrees& next) {
                next.addTrack(s, t, std::make_unique<Track>(model::defaultTrack(t)));
                if (withSequences)
                    for (int q = 0; q < model::kPatterns; ++q)
                        editPattern(model, next, s, q, [&](Pattern& np) { clearCells(np, trackBit(t), kAllSteps, true); });
            });
        }
        case ObjectKind::Sequence:
            return std::make_unique<EditCommand>("CLEAR SEQUENCE", [s, p, t](const Project& model, Subtrees& next) {
                editPattern(model, next, s, p, [&](Pattern& np) { clearCells(np, trackBit(t), kAllSteps, true); });
            });
        case ObjectKind::Steps:
        case ObjectKind::Page:
        {
            const std::uint16_t tracks = trackMaskOf(r);
            const std::uint64_t mask = r.stepMask;
            return std::make_unique<EditCommand>("CLEAR STEPS", [s, p, tracks, mask](const Project& model, Subtrees& next) {
                editPattern(model, next, s, p, [&](Pattern& np) { clearCells(np, tracks, mask, false); });
            });
        }
    }
    return nullptr;
}

std::unique_ptr<Command> makeSwap(const EditRef& a, const EditRef& b)
{
    const Family f = familyOf(a.kind);
    if (f != familyOf(b.kind))
        return nullptr;
    switch (f)
    {
        case Family::Scene:
        case Family::Pattern: return std::make_unique<SwapSlotsCommand>(a, b);
        case Family::Track:
        {
            const bool withSequences =
                a.kind == ObjectKind::TrackWithSequences || b.kind == ObjectKind::TrackWithSequences;
            if (! withSequences)
                return std::make_unique<SwapSlotsCommand>(a, b);
            if ((a.scene & 15) == (b.scene & 15))
                return std::make_unique<SwapTracksCommand>(a.scene, a.track, b.track, true); // EDT-002
            const int sa = a.scene & 15, sb = b.scene & 15, ta = a.track & 15, tb = b.track & 15;
            return std::make_unique<EditCommand>("SWAP TRACK", [=](const Project& model, Subtrees& next) {
                next.addTrack(sa, ta, std::make_unique<Track>(model.scenes[at(sb)].tracks[at(tb)]));
                next.addTrack(sb, tb, std::make_unique<Track>(model.scenes[at(sa)].tracks[at(ta)]));
                for (int q = 0; q < model::kPatterns; ++q)
                {
                    const auto ca = extractCells(patternOf(model, sa, q), trackBit(ta), kAllSteps, true);
                    const auto cb = extractCells(patternOf(model, sb, q), trackBit(tb), kAllSteps, true);
                    editPattern(model, next, sa, q, [&](Pattern& np) { writeCells(np, cb, nullptr, ta); });
                    editPattern(model, next, sb, q, [&](Pattern& np) { writeCells(np, ca, nullptr, tb); });
                }
            });
        }
        case Family::Sequence:
        {
            const int sa = a.scene & 15, sb = b.scene & 15, pa = a.pattern & 15, pb = b.pattern & 15;
            const int ta = a.track & 15, tb = b.track & 15;
            return std::make_unique<EditCommand>("SWAP SEQUENCE", [=](const Project& model, Subtrees& next) {
                if (sa == sb && pa == pb)
                {
                    editPattern(model, next, sa, pa, [&](Pattern& np) { swapColumns(np, ta, tb); });
                    return;
                }
                const auto ca = extractCells(patternOf(model, sa, pa), trackBit(ta), kAllSteps, true);
                const auto cb = extractCells(patternOf(model, sb, pb), trackBit(tb), kAllSteps, true);
                editPattern(model, next, sa, pa, [&](Pattern& np) { writeCells(np, cb, nullptr, ta); });
                editPattern(model, next, sb, pb, [&](Pattern& np) { writeCells(np, ca, nullptr, tb); });
            });
        }
        case Family::Steps: break;
    }
    return nullptr;
}
} // namespace core::edit
