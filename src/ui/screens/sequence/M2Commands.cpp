#include "ui/screens/sequence/M2Commands.h"

#include "core/commands/scene/SceneCommands.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/edit/EditCommands.h"
#include "core/model/ModelOps.h"
#include "ui/screens/sequence/M2Session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ui::m2::cmd
{
namespace
{
using core::model::LockSlot;
using core::model::Pattern;
using core::model::Scene;
using core::model::Step;
using core::model::StepLocks;
using core::model::Track;

std::string path(const char* fmt, int a, int b = 0)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    return buf;
}

// Drops lock entries no cell references (or with an empty mask) and renumbers the cells' indices.
void compactLocks(Pattern& p)
{
    std::vector<StepLocks> kept;
    kept.reserve(p.locks.size());
    for (auto& row : p.steps)
        for (auto& cell : row)
        {
            if (cell.lockIndex == Step::kNoLocks)
                continue;
            if (cell.lockIndex >= p.locks.size() || p.locks[cell.lockIndex].mask == 0)
            {
                cell.lockIndex = Step::kNoLocks;
                continue;
            }
            kept.push_back(p.locks[cell.lockIndex]);
            cell.lockIndex = static_cast<std::uint16_t>(kept.size() - 1);
        }
    p.locks = std::move(kept);
}

void clearCell(Step& cell)
{
    cell.trigger = Step::kEmpty;
    cell.value2 = Step::kNoValue2;
    cell.triggerType = core::model::TriggerType::Full;
    cell.lockIndex = Step::kNoLocks;
}

bool validStep(int step) noexcept
{
    return step >= 0 && step < core::model::kSteps;
}

// Replaces scene[s].pattern[p] (the new value computed by the factory) — inverse = the previous pattern.
class PatternSet final : public core::Command
{
public:
    PatternSet(int scene, int pattern, std::unique_ptr<Pattern> next, std::string_view label)
        : scene_(scene), pattern_(pattern), next_(std::move(next)), label_(label)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        p.scenes[static_cast<std::size_t>(scene_)].patterns[static_cast<std::size_t>(pattern_)] = *next_;
    }
    std::unique_ptr<core::Command> inverse(const Project& before) const override
    {
        return std::make_unique<PatternSet>(
            scene_, pattern_,
            std::make_unique<Pattern>(
                before.scenes[static_cast<std::size_t>(scene_)].patterns[static_cast<std::size_t>(pattern_)]),
            label_);
    }
    void collectChanges(core::ChangeSet& c) const override { c.add(path("scene[%d].pattern[%d]", scene_, pattern_)); }
    std::size_t sizeBytes() const override
    {
        return sizeof(*this) + sizeof(Pattern) + next_->locks.size() * sizeof(StepLocks);
    }

private:
    int scene_, pattern_;
    std::unique_ptr<Pattern> next_;
    std::string_view label_;
};

// One track of a scene and its 16 patterns (the TRACK tab's copy / paste / reset).
class TrackSet final : public core::Command
{
public:
    TrackSet(int scene, int track, std::unique_ptr<Track> next,
             std::unique_ptr<std::array<Pattern, core::model::kPatterns>> patterns, std::string_view label)
        : scene_(scene), track_(track), next_(std::move(next)), patterns_(std::move(patterns)), label_(label)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        auto& s = p.scenes[static_cast<std::size_t>(scene_)];
        s.tracks[static_cast<std::size_t>(track_)] = *next_;
        if (patterns_)
            s.patterns = *patterns_;
    }
    std::unique_ptr<core::Command> inverse(const Project& before) const override
    {
        const auto& s = before.scenes[static_cast<std::size_t>(scene_)];
        std::unique_ptr<std::array<Pattern, core::model::kPatterns>> patterns;
        if (patterns_)
            patterns = std::make_unique<std::array<Pattern, core::model::kPatterns>>(s.patterns);
        return std::make_unique<TrackSet>(scene_, track_,
                                          std::make_unique<Track>(s.tracks[static_cast<std::size_t>(track_)]),
                                          std::move(patterns), label_);
    }
    void collectChanges(core::ChangeSet& c) const override
    {
        c.add(path("scene[%d].track[%d]", scene_, track_));
        if (patterns_)
            for (int p = 0; p < core::model::kPatterns; ++p)
                c.add(path("scene[%d].pattern[%d]", scene_, p));
    }
    std::size_t sizeBytes() const override
    {
        return sizeof(*this) + sizeof(Track) + (patterns_ ? sizeof(*patterns_) : 0);
    }

private:
    int scene_, track_;
    std::unique_ptr<Track> next_;
    std::unique_ptr<std::array<Pattern, core::model::kPatterns>> patterns_;
    std::string_view label_;
};

std::unique_ptr<Pattern> copyOf(const Project& p, int scene, int pattern)
{
    return std::make_unique<Pattern>(
        p.scenes[static_cast<std::size_t>(scene & 15)].patterns[static_cast<std::size_t>(pattern & 15)]);
}

Step& cellOf(Pattern& p, int step, int track)
{
    return p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track & 15)];
}
} // namespace

std::unique_ptr<core::Command> toggleStep(const Project&, int scene, int pattern, int track, int step,
                                          core::model::TriggerType type, int velocity)
{
    return core::commands::sequencer::toggleStep(scene, pattern, track, step, type, velocity);
}

std::unique_ptr<core::Command> setLocks(const Project&, int scene, int pattern, int track, std::span<const int> steps,
                                        LockSlot slot, std::span<const float> values, float removeAt,
                                        std::string_view label)
{
    return core::commands::sequencer::setLocks(scene, pattern, track, steps, slot, values, removeAt, label);
}

std::unique_ptr<core::Command> pasteSteps(const Project& project, int scene, int pattern, std::uint16_t trackMask,
                                          int first, int count, const Pattern* source, int srcFirst)
{
    auto next = copyOf(project, scene, pattern);
    for (int t = 0; t < core::model::kTracks; ++t)
    {
        if ((trackMask >> t & 1u) == 0)
            continue;
        for (int i = 0; i < count; ++i)
        {
            const int to = first + i, from = srcFirst + i;
            if (! validStep(to))
                continue;
            auto& cell = cellOf(*next, to, t);
            clearCell(cell);
            if (source == nullptr || ! validStep(from))
                continue;
            const auto& src = source->steps[static_cast<std::size_t>(from)][static_cast<std::size_t>(t)];
            cell = src;
            cell.lockIndex = Step::kNoLocks;
            if (src.lockIndex != Step::kNoLocks && src.lockIndex < source->locks.size())
            {
                next->locks.push_back(source->locks[src.lockIndex]);
                cell.lockIndex = static_cast<std::uint16_t>(next->locks.size() - 1);
            }
        }
    }
    compactLocks(*next);
    return std::make_unique<PatternSet>(scene, pattern, std::move(next), source != nullptr ? "PASTE" : "DELETE");
}

// The scene / pattern commands are core::commands::scene's; these keep the ui::m2 call sites compiling.
std::unique_ptr<core::Command> replaceScene(int dst, const Scene& source)
{
    return core::commands::scene::replaceScene(dst, source);
}

std::unique_ptr<core::Command> replacePattern(int scene, int dst, const Pattern& source)
{
    return core::commands::scene::replacePattern(scene, dst, source);
}

std::unique_ptr<core::Command> clearScene(const Project&, int scene)
{
    return core::commands::scene::clearScene(scene);
}

std::unique_ptr<core::Command> clearPattern(int scene, int pattern)
{
    return core::commands::scene::clearPattern(scene, pattern);
}

std::unique_ptr<core::Command> setSceneName(int scene, std::string name)
{
    return core::commands::scene::setSceneName(scene, std::move(name));
}

std::unique_ptr<core::Command> setSceneColour(int scene, int colour)
{
    return core::commands::scene::setSceneColour(scene, colour);
}

std::unique_ptr<core::Command> setSceneBpm(int scene, std::optional<double> bpm)
{
    return core::commands::scene::setSceneBpm(scene, bpm);
}

std::unique_ptr<core::Command> setPatternLength(const Project& p, int scene, int pattern, int steps)
{
    return core::commands::scene::resizePattern(p, scene, pattern, steps); // longer: repeats the bars
}

std::unique_ptr<core::Command> replaceTrack(const Project& project, int scene, int track, const Track& source,
                                            const Scene* sequenceFrom, int sourceTrack)
{
    std::unique_ptr<std::array<Pattern, core::model::kPatterns>> patterns;
    if (sequenceFrom != nullptr)
    {
        patterns = std::make_unique<std::array<Pattern, core::model::kPatterns>>(
            project.scenes[static_cast<std::size_t>(scene)].patterns);
        for (std::size_t p = 0; p < patterns->size(); ++p)
        {
            auto& dst = (*patterns)[p];
            const auto& src = sequenceFrom->patterns[p];
            for (int s = 0; s < core::model::kSteps; ++s)
            {
                const auto& from = src.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(sourceTrack & 15)];
                auto& to = cellOf(dst, s, track);
                to = from;
                to.lockIndex = Step::kNoLocks;
                if (from.lockIndex != Step::kNoLocks && from.lockIndex < src.locks.size())
                {
                    dst.locks.push_back(src.locks[from.lockIndex]);
                    to.lockIndex = static_cast<std::uint16_t>(dst.locks.size() - 1);
                }
            }
            compactLocks(dst);
        }
    }
    return std::make_unique<TrackSet>(scene, track, std::make_unique<Track>(source), std::move(patterns),
                                      "PASTE TRACK");
}

std::unique_ptr<core::Command> resetTrack(const Project& project, int scene, int track, bool withSequence)
{
    std::unique_ptr<std::array<Pattern, core::model::kPatterns>> patterns;
    if (withSequence)
    {
        patterns = std::make_unique<std::array<Pattern, core::model::kPatterns>>(
            project.scenes[static_cast<std::size_t>(scene)].patterns);
        for (auto& p : *patterns)
        {
            for (int s = 0; s < core::model::kSteps; ++s)
                clearCell(cellOf(p, s, track));
            compactLocks(p);
        }
    }
    return std::make_unique<TrackSet>(scene, track, std::make_unique<Track>(core::model::defaultTrack(track)),
                                      std::move(patterns), "DELETE TRACK");
}
void exchangeTrackSlots(int scene, int source, int destination)
{
    const auto a = core::edit::trackRef(scene, source, true);
    const auto b = core::edit::trackRef(scene, destination, true);
    if (auto change = core::edit::makeSwap(a, b))
        Session::instance().execute(std::move(change));
}
} // namespace ui::m2::cmd
