#include "core/commands/scene/SceneCommands.h"

#include "core/commands/SetParam.h"
#include "core/model/ModelOps.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>

namespace core::commands::scene
{
namespace
{
using model::ParamPath;
using model::Pattern;
using model::Project;
using model::Scene;

std::size_t at(int i) noexcept
{
    return static_cast<std::size_t>(i & 15);
}

std::string path(const char* fmt, int a, int b = 0)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    return buf;
}

// A switch request: the model is not touched, only the path tells the bridge what to post.
class QueueRequest final : public core::Command
{
public:
    explicit QueueRequest(int value) : value_(value) {}
    std::string_view label() const override { return "SCENE"; }
    void apply(Project&) override {}
    std::unique_ptr<core::Command> inverse(const Project&) const override { return nullptr; }
    bool undoable() const override { return false; }
    void collectChanges(core::ChangeSet& c) const override { c.add(queuePath(value_)); }

private:
    int value_;
};

class CurrentSet final : public core::Command
{
public:
    CurrentSet(int scene, int pattern) : scene_(scene & 15), pattern_(pattern & 15) {}
    std::string_view label() const override { return "SCENE"; }
    void apply(Project& p) override
    {
        p.settings.currentScene = scene_;
        p.settings.currentPattern = pattern_;
    }
    std::unique_ptr<core::Command> inverse(const Project&) const override { return nullptr; }
    bool undoable() const override { return false; }
    void collectChanges(core::ChangeSet& c) const override
    {
        c.add("settings.currentScene");
        c.add("settings.currentPattern");
    }

private:
    int scene_, pattern_;
};

// Replaces scene[s] (or, with `reset`, resets it to the defaults at apply time) — inverse = the previous scene.
class SceneSet final : public core::Command
{
public:
    SceneSet(int scene, std::unique_ptr<Scene> next, std::string_view label)
        : scene_(scene & 15), next_(std::move(next)), label_(label)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        if (next_)
            p.scenes[at(scene_)] = *next_;
        else
            model::resetScene(p.scenes[at(scene_)], scene_);
    }
    std::unique_ptr<core::Command> inverse(const Project& before) const override
    {
        return std::make_unique<SceneSet>(scene_, std::make_unique<Scene>(before.scenes[at(scene_)]), label_);
    }
    void collectChanges(core::ChangeSet& c) const override { c.add(path("scene[%d]", scene_)); }
    std::size_t sizeBytes() const override { return sizeof(*this) + (next_ ? sizeof(Scene) : 0); }

private:
    int scene_;
    std::unique_ptr<Scene> next_; // null = reset (clearScene)
    std::string_view label_;
};

class PatternSet final : public core::Command
{
public:
    PatternSet(int scene, int pattern, std::unique_ptr<Pattern> next, std::string_view label)
        : scene_(scene & 15), pattern_(pattern & 15), next_(std::move(next)), label_(label)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(Project& p) override { p.scenes[at(scene_)].patterns[at(pattern_)] = *next_; }
    std::unique_ptr<core::Command> inverse(const Project& before) const override
    {
        return std::make_unique<PatternSet>(
            scene_, pattern_, std::make_unique<Pattern>(before.scenes[at(scene_)].patterns[at(pattern_)]), label_);
    }
    void collectChanges(core::ChangeSet& c) const override
    {
        c.add(path("scene[%d].pattern[%d]", scene_, pattern_));
    }
    std::size_t sizeBytes() const override
    {
        return sizeof(*this) + sizeof(Pattern) + next_->locks.size() * sizeof(model::StepLocks);
    }

private:
    int scene_, pattern_;
    std::unique_ptr<Pattern> next_;
    std::string_view label_;
};

constexpr auto kLength = [](Project& p, const ParamPath& a) -> int& {
    return p.scenes[at(a.scene)].patterns[at(a.pattern)].length;
};
constexpr auto kName = [](Project& p, const ParamPath& a) -> std::string& { return p.scenes[at(a.scene)].name; };
constexpr auto kColour = [](Project& p, const ParamPath& a) -> int& { return p.scenes[at(a.scene)].colour; };
constexpr auto kBpm = [](Project& p, const ParamPath& a) -> std::optional<double>& {
    return p.scenes[at(a.scene)].bpm;
};

ParamPath scenePath(int scene, model::Scope scope = model::Scope::Scene, int pattern = 0)
{
    ParamPath p {};
    p.scope = scope;
    p.scene = static_cast<std::uint8_t>(scene & 15);
    p.pattern = static_cast<std::uint8_t>(pattern & 15);
    return p;
}

std::array<char, 64> nameText {};
std::size_t nameLength = 0;
} // namespace

std::string queuePath(int value)
{
    return path("queue[%d]", value);
}

std::optional<int> queueValueOf(std::string_view p) noexcept
{
    constexpr std::string_view prefix = "queue[";
    if (p.size() < prefix.size() + 2 || p.substr(0, prefix.size()) != prefix || p.back() != ']')
        return std::nullopt;
    const auto digits = p.substr(prefix.size(), p.size() - prefix.size() - 1);
    int v = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (ec != std::errc {} || end != digits.data() + digits.size())
        return std::nullopt;
    return v;
}

std::unique_ptr<core::Command> selectScenePattern(int scene, int pattern)
{
    return selectScenePattern(scene, pattern, false);
}

std::unique_ptr<core::Command> selectScenePattern(int scene, int pattern, bool applyBpm)
{
    return std::make_unique<QueueRequest>((scene & 15) * 16 + (pattern & 15) + (applyBpm ? kQueueApplyBpm : 0));
}

std::unique_ptr<core::Command> cancelScenePattern()
{
    return std::make_unique<QueueRequest>(kQueueCancel);
}

std::unique_ptr<core::Command> setSwitchable(bool on)
{
    return std::make_unique<QueueRequest>(on ? kQueueUnblock : kQueueBlock);
}

std::unique_ptr<core::Command> setCurrentScenePattern(int scene, int pattern)
{
    return std::make_unique<CurrentSet>(scene, pattern);
}

std::unique_ptr<core::Command> setPatternLength(int scene, int pattern, int steps)
{
    return makeSetParam<int>(path("scene[%d].pattern[%d].length", scene & 15, pattern & 15), "PATTERN LENGTH", kLength,
                             scenePath(scene, model::Scope::Pattern, pattern), std::clamp(steps, 1, model::kSteps));
}

std::unique_ptr<core::Command> setSceneName(int scene, std::string name)
{
    return makeSetParam<std::string>(path("scene[%d].name", scene & 15), "SCENE NAME", kName, scenePath(scene),
                                     std::move(name), SetParam<std::string>::Coalesce::No);
}

std::unique_ptr<core::Command> setSceneColour(int scene, int colour)
{
    return makeSetParam<int>(path("scene[%d].colour", scene & 15), "COLOR", kColour, scenePath(scene),
                             std::clamp(colour, 0, 15));
}

std::unique_ptr<core::Command> setSceneBpm(int scene, std::optional<double> bpm)
{
    if (bpm && ! (*bpm > 0.0))
        bpm.reset(); // 0 / OFF = no scene BPM
    return makeSetParam<std::optional<double>>(path("scene[%d].bpm", scene & 15), "SCENE BPM", kBpm, scenePath(scene),
                                               bpm);
}

std::unique_ptr<core::Command> replaceScene(int dst, const Scene& source)
{
    return std::make_unique<SceneSet>(dst, std::make_unique<Scene>(source), "PASTE SCENE");
}

std::unique_ptr<core::Command> replacePattern(int scene, int dst, const Pattern& source)
{
    return std::make_unique<PatternSet>(scene, dst, std::make_unique<Pattern>(source), "PASTE PATTERN");
}

std::unique_ptr<core::Command> resizePattern(const Project& p, int scene, int pattern, int steps)
{
    const Pattern& src = p.scenes[at(scene & 15)].patterns[at(pattern & 15)];
    const int from = std::clamp(src.length, 1, model::kSteps), to = std::clamp(steps, 1, model::kSteps);
    const auto cells = [&src](int a, int b)
    {
        for (int s = a; s < b; ++s)
            for (const auto& c : src.steps[static_cast<std::size_t>(s)])
                if (! c.empty())
                    return true;
        return false;
    };
    if (to <= from || cells(from, to) || ! cells(0, from))
        return setPatternLength(scene, pattern, steps);
    // The repeat period: the whole bars of the old length (one step at a time from 16 → 32 still copies bar 1 step
    // by step), the old length itself under one bar.
    const int period = from >= 16 ? from / 16 * 16 : from;
    auto next = std::make_unique<Pattern>(src);
    next->length = to;
    for (int s = from; s < to; ++s)
        for (int t = 0; t < model::kTracks; ++t)
        {
            auto cell = src.steps[static_cast<std::size_t>(s % period)][at(t)];
            if (cell.lockIndex != model::Step::kNoLocks)
            {
                if (cell.lockIndex < src.locks.size() && next->locks.size() < model::Step::kNoLocks)
                {
                    next->locks.push_back(src.locks[cell.lockIndex]); // its own copy: later edits stay per step
                    cell.lockIndex = static_cast<std::uint16_t>(next->locks.size() - 1);
                }
                else
                    cell.lockIndex = model::Step::kNoLocks;
            }
            next->steps[static_cast<std::size_t>(s)][at(t)] = cell; // (at() masks to 0–15: steps go to 63)
        }
    return std::make_unique<PatternSet>(scene, pattern, std::move(next), "PATTERN LENGTH");
}

std::unique_ptr<core::Command> clearScene(int scene)
{
    return std::make_unique<SceneSet>(scene, nullptr, "DELETE SCENE");
}

std::unique_ptr<core::Command> clearPattern(int scene, int pattern)
{
    return std::make_unique<PatternSet>(scene, pattern, std::make_unique<Pattern>(model::defaultPattern()),
                                        "DELETE PATTERN");
}

bool patternHasContent(const Pattern& p) noexcept
{
    for (const auto& row : p.steps)
        for (const auto& cell : row)
            if (cell.trigger != model::Step::kEmpty)
                return true;
    return false;
}

std::uint16_t patternContentMask(const Scene& s) noexcept
{
    std::uint16_t mask = 0;
    for (int p = 0; p < model::kPatterns; ++p)
        if (patternHasContent(s.patterns[static_cast<std::size_t>(p)]))
            mask = static_cast<std::uint16_t>(mask | 1u << p);
    return mask;
}

bool sceneHasContent(const Scene& s) noexcept
{
    // As the SCENE MANAGER keys: a recorded pattern, a name or a scene BPM.
    return ! s.name.empty() || s.bpm.has_value() || patternContentMask(s) != 0;
}

std::string_view focusedSceneName() noexcept
{
    return {nameText.data(), nameLength};
}

void setFocusedSceneName(std::string_view name) noexcept
{
    nameLength = std::min(name.size(), nameText.size());
    std::copy_n(name.data(), nameLength, nameText.data());
}
} // namespace core::commands::scene
