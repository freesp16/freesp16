#include "core/edit/EditOps.h"

#include "core/params/ParamRegistry.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <string>

namespace core::edit
{
namespace
{
using model::Pattern;
using model::Project;
using model::Scene;
using model::Step;
using model::StepLocks;
using model::Track;

constexpr std::size_t at(int i) noexcept
{
    return static_cast<std::size_t>(i & 15);
}

bool hasLocks(const Pattern& p, const Step& s) noexcept
{
    return s.lockIndex != Step::kNoLocks && s.lockIndex < p.locks.size();
}

// ---- path expansion ----
// The registered model parameters per slot kind, as path tails (formOf rules of core/model/paths/ParamPath.cpp):
// track ids "track.x" → "x", other track ids as they are; FX insert ids (fx1. / fx2.) are covered by the coarse
// "insertFx[n]" paths the FX sync reads; scene ids "scene.x" → "x", send. / master. / pattern. ids are covered by the
// coarse paths. Internal ids (storage key "internal.*", e.g. scene.queue) are never reported.
struct Tails
{
    std::vector<std::string> track, scene;
};

bool startsWith(std::string_view s, std::string_view p) noexcept
{
    return s.substr(0, p.size()) == p;
}

const Tails& tails()
{
    static const Tails t = [] {
        Tails out;
        for (const auto& spec : core::builtinParams().all())
        {
            const std::string_view id = spec.id;
            if (startsWith(spec.storageKey(), "internal."))
                continue;
            switch (spec.scope)
            {
                case ParamScope::Track:
                    if (startsWith(id, "fx1.") || startsWith(id, "fx2."))
                        break;
                    out.track.emplace_back(startsWith(id, "track.") ? id.substr(6) : id);
                    break;
                case ParamScope::Scene:
                case ParamScope::SceneOverrideElseProject:
                    if (startsWith(id, "send.") || startsWith(id, "master.") || startsWith(id, "pattern."))
                        break;
                    out.scene.emplace_back(startsWith(id, "scene.") ? id.substr(6) : id);
                    break;
                default: break;
            }
        }
        return out;
    }();
    return t;
}

std::string prefix(const char* fmt, int a, int b = 0)
{
    char buf[48];
    const int n = std::snprintf(buf, sizeof buf, fmt, a, b);
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}
} // namespace

// ---- cells ----

CellSet extractCells(const Pattern& p, std::uint16_t trackMask, std::uint64_t stepMask, bool withFlags)
{
    CellSet out;
    out.stepMask = stepMask;
    out.cells.reserve(static_cast<std::size_t>(std::popcount(stepMask)) * static_cast<std::size_t>(std::popcount(trackMask)));
    for (int s = 0; s < model::kSteps; ++s)
    {
        if ((stepMask >> s & 1u) == 0)
            continue;
        for (int t = 0; t < model::kTracks; ++t)
        {
            if ((trackMask >> t & 1u) == 0)
                continue;
            Step cell = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            if (hasLocks(p, cell))
            {
                out.locks.push_back(p.locks[cell.lockIndex]);
                cell.lockIndex = static_cast<std::uint16_t>(out.locks.size() - 1);
            }
            else
                cell.lockIndex = Step::kNoLocks;
            out.cells.push_back({static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(t), cell});
        }
    }
    if (withFlags)
    {
        out.flagTracks = trackMask;
        for (int t = 0; t < model::kTracks; ++t)
            if ((trackMask >> t & 1u) != 0)
            {
                out.mutes[at(t)] = p.trackMutes[at(t)];
                out.solos[at(t)] = p.trackSolos[at(t)];
            }
    }
    return out;
}

void writeCells(Pattern& dst, const CellSet& set, const std::function<int(int)>& place, int trackTo)
{
    for (const auto& c : set.cells)
    {
        const int s = place ? place(c.step) : c.step;
        if (s < 0 || s >= model::kSteps)
            continue;
        const int t = trackTo < 0 ? c.track : (trackTo & 15);
        Step cell = c.value;
        if (cell.lockIndex != Step::kNoLocks && cell.lockIndex < set.locks.size())
        {
            dst.locks.push_back(set.locks[cell.lockIndex]);
            cell.lockIndex = static_cast<std::uint16_t>(dst.locks.size() - 1); // the old record is dropped below
        }
        else
            cell.lockIndex = Step::kNoLocks;
        dst.steps[static_cast<std::size_t>(s)][at(t)] = cell;
    }
    for (int t = 0; t < model::kTracks; ++t)
        if ((set.flagTracks >> t & 1u) != 0)
        {
            const int to = trackTo < 0 ? t : (trackTo & 15);
            dst.trackMutes[at(to)] = set.mutes[at(t)];
            dst.trackSolos[at(to)] = set.solos[at(t)];
        }
    compactLocks(dst);
}

void clearCells(Pattern& p, std::uint16_t trackMask, std::uint64_t stepMask, bool flags)
{
    for (int s = 0; s < model::kSteps; ++s)
        if ((stepMask >> s & 1u) != 0)
            for (int t = 0; t < model::kTracks; ++t)
                if ((trackMask >> t & 1u) != 0)
                    p.steps[static_cast<std::size_t>(s)][at(t)] = Step {};
    if (flags)
        for (int t = 0; t < model::kTracks; ++t)
            if ((trackMask >> t & 1u) != 0)
                p.trackMutes[at(t)] = p.trackSolos[at(t)] = false;
    compactLocks(p);
}

void swapColumns(Pattern& p, int a, int b) noexcept
{
    if ((a & 15) == (b & 15))
        return;
    for (auto& row : p.steps)
        std::swap(row[at(a)], row[at(b)]);
    std::swap(p.trackMutes[at(a)], p.trackMutes[at(b)]);
    std::swap(p.trackSolos[at(a)], p.trackSolos[at(b)]);
}

void compactLocks(Pattern& p)
{
    bool compact = true; // already ordered by first use and fully referenced? (the common case: nothing to move)
    std::uint16_t next = 0;
    for (auto& row : p.steps)
        for (auto& cell : row)
        {
            if (cell.lockIndex == Step::kNoLocks)
                continue;
            if (cell.lockIndex >= p.locks.size())
            {
                cell.lockIndex = Step::kNoLocks;
                continue;
            }
            if (cell.lockIndex != next)
                compact = false;
            ++next;
        }
    if (compact && next == p.locks.size())
        return;
    std::vector<StepLocks> locks;
    locks.reserve(next);
    for (auto& row : p.steps)
        for (auto& cell : row)
            if (cell.lockIndex != Step::kNoLocks)
            {
                locks.push_back(p.locks[cell.lockIndex]);
                cell.lockIndex = static_cast<std::uint16_t>(locks.size() - 1);
            }
    p.locks = std::move(locks);
}

int lowestStep(std::uint64_t mask) noexcept
{
    return mask == 0 ? 0 : std::countr_zero(mask);
}

int highestStep(std::uint64_t mask) noexcept
{
    return mask == 0 ? 0 : 63 - std::countl_zero(mask);
}

int placeStep(int step, std::uint64_t srcMask, int dstFirst, int dstLength) noexcept
{
    const int srcFirst = lowestStep(srcMask);
    const int span = highestStep(srcMask) - srcFirst + 1;
    const int len = std::clamp(dstLength, 1, model::kSteps);
    const int pos = dstFirst + (step - srcFirst);
    if (pos < 0)
        return -1;
    return span <= len ? pos % len : pos % model::kSteps;
}

// ---- sizes ----

static std::size_t extrasBytes(const model::Extras& e) noexcept
{
    std::size_t n = e.entries.capacity() * sizeof(model::Extras::Entry);
    for (const auto& x : e.entries)
        n += x.key.capacity() + x.value.capacity();
    return n;
}

std::size_t deepBytes(const Pattern& p) noexcept
{
    return sizeof(Pattern) + p.locks.capacity() * sizeof(StepLocks) + extrasBytes(p.extras);
}

std::size_t deepBytes(const Track& t) noexcept
{
    return sizeof(Track) + t.name.capacity() + t.slices.grids.capacity() * sizeof(std::int64_t) + extrasBytes(t.extras);
}

std::size_t deepBytes(const Scene& s) noexcept
{
    std::size_t n = sizeof(Scene) + s.name.capacity() + extrasBytes(s.extras);
    for (const auto& p : s.patterns)
        n += deepBytes(p) - sizeof(Pattern);
    for (const auto& t : s.tracks)
        n += deepBytes(t) - sizeof(Track);
    return n;
}

// ---- paths ----

void addTrackPaths(ChangeSet& c, int scene, int track)
{
    const std::string head = prefix("scene[%d].track[%d].", scene & 15, track & 15);
    for (const auto& tail : tails().track)
        c.add(head + tail);
    c.add(head + "insertFx[0]");
    c.add(head + "insertFx[1]");
}

void addPatternPath(ChangeSet& c, int scene, int pattern)
{
    c.add(prefix("scene[%d].pattern[%d]", scene & 15, pattern & 15));
}

void addSceneFieldPaths(ChangeSet& c, int scene)
{
    const std::string head = prefix("scene[%d].", scene & 15);
    for (const auto& tail : tails().scene)
        c.add(head + tail);
    c.add(head + "sendFx");
    c.add(head + "masterFx");
    c.add(head + "routing.fxMode");
}

void addScenePaths(ChangeSet& c, int scene, std::uint16_t patterns, std::uint16_t tracks)
{
    addSceneFieldPaths(c, scene);
    for (int p = 0; p < model::kPatterns; ++p)
        if ((patterns >> p & 1u) != 0)
            addPatternPath(c, scene, p);
    for (int t = 0; t < model::kTracks; ++t)
        if ((tracks >> t & 1u) != 0)
            addTrackPaths(c, scene, t);
}

// ---- Subtrees ----

void Subtrees::addScene(int scene, std::unique_ptr<Scene> v)
{
    scenes_.push_back({scene & 15, std::move(v)});
}

void Subtrees::addPattern(int scene, int pattern, std::unique_ptr<Pattern> v)
{
    patterns_.push_back({scene & 15, pattern & 15, std::move(v)});
}

void Subtrees::addTrack(int scene, int track, std::unique_ptr<Track> v)
{
    tracks_.push_back({scene & 15, track & 15, std::move(v)});
}

void Subtrees::exchange(Project& p) noexcept
{
    for (auto& s : scenes_)
    {
        auto& now = p.scenes[at(s.scene)];
        std::swap(now, *s.value);
        s.changedPatterns = s.changedTracks = 0;
        for (int i = 0; i < model::kPatterns; ++i)
            if (! (now.patterns[at(i)] == s.value->patterns[at(i)]))
                s.changedPatterns = static_cast<std::uint16_t>(s.changedPatterns | 1u << i);
        for (int i = 0; i < model::kTracks; ++i)
            if (! (now.tracks[at(i)] == s.value->tracks[at(i)]))
                s.changedTracks = static_cast<std::uint16_t>(s.changedTracks | 1u << i);
    }
    for (auto& s : patterns_)
    {
        auto& now = p.scenes[at(s.scene)].patterns[at(s.pattern)];
        std::swap(now, *s.value);
        s.changed = ! (now == *s.value);
    }
    for (auto& s : tracks_)
    {
        auto& now = p.scenes[at(s.scene)].tracks[at(s.track)];
        std::swap(now, *s.value);
        s.changed = ! (now == *s.value);
    }
}

std::size_t Subtrees::bytes() const noexcept
{
    std::size_t n = sizeof(*this);
    for (const auto& s : scenes_)
        n += deepBytes(*s.value);
    for (const auto& s : patterns_)
        n += deepBytes(*s.value);
    for (const auto& s : tracks_)
        n += deepBytes(*s.value);
    return n;
}

void Subtrees::collect(ChangeSet& c) const
{
    for (const auto& s : scenes_)
        addScenePaths(c, s.scene, s.changedPatterns, s.changedTracks);
    for (const auto& s : patterns_)
        if (s.changed)
            addPatternPath(c, s.scene, s.pattern);
    for (const auto& s : tracks_)
        if (s.changed)
            addTrackPaths(c, s.scene, s.track);
}

// ---- commands ----

namespace
{
class ExchangeBack final : public Command
{
public:
    ExchangeBack(std::string_view label, std::shared_ptr<Subtrees> holder) : label_(label), holder_(std::move(holder)) {}
    std::string_view label() const override { return label_; }
    void apply(Project& p) override { holder_->exchange(p); }
    std::unique_ptr<Command> inverse(const Project&) const override
    {
        return std::make_unique<ExchangeBack>(label_, holder_);
    }
    void collectChanges(ChangeSet& c) const override { holder_->collect(c); }
    std::size_t sizeBytes() const override { return sizeof(*this) + holder_->bytes(); }

private:
    std::string_view label_;
    std::shared_ptr<Subtrees> holder_;
};
} // namespace

EditCommand::EditCommand(std::string_view label, Build build)
    : label_(label), build_(std::move(build)), holder_(std::make_shared<Subtrees>())
{
}

void EditCommand::apply(Project& p)
{
    if (build_)
    {
        build_(p, *holder_);
        build_ = nullptr;
    }
    holder_->exchange(p);
}

std::unique_ptr<Command> EditCommand::inverse(const Project&) const
{
    return std::make_unique<ExchangeBack>(label_, holder_);
}

void SwapTracksCommand::apply(Project& p)
{
    auto& s = p.scenes[at(scene_)];
    if (a_ == b_)
        return;
    std::swap(s.tracks[at(a_)], s.tracks[at(b_)]);
    if (withSequences_)
        for (auto& pattern : s.patterns)
            swapColumns(pattern, a_, b_);
}

void SwapTracksCommand::collectChanges(ChangeSet& c) const
{
    addTrackPaths(c, scene_, a_);
    addTrackPaths(c, scene_, b_);
    if (withSequences_)
        for (int pt = 0; pt < model::kPatterns; ++pt)
            addPatternPath(c, scene_, pt);
}
} // namespace core::edit
