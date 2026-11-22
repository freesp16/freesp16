#include "ui/input/scenes/SceneSync.h"

#include "core/commands/scene/SceneCommands.h"
#include "core/model/Project.h"

#include <charconv>
#include <string_view>

namespace ui::input::scenes
{
namespace
{
namespace m = core::model;

// Reads "[<int>]" at the front of `s`; advances it. False when absent.
bool index(std::string_view& s, int& out) noexcept
{
    if (s.empty() || s.front() != '[')
        return false;
    const auto close = s.find(']');
    if (close == std::string_view::npos)
        return false;
    const auto [end, ec] = std::from_chars(s.data() + 1, s.data() + close, out);
    if (ec != std::errc {} || end != s.data() + close || out < 0 || out > 15)
        return false;
    s.remove_prefix(close + 1);
    return true;
}

bool startsWith(std::string_view s, std::string_view prefix) noexcept
{
    return s.substr(0, prefix.size()) == prefix;
}
} // namespace

SceneSync::SceneSync(const core::ParamRegistry& params, engine::EngineBridge& bridge)
    : bridge_(bridge),
      queue_(params.idOf("scene.queue")),
      bpm_(params.idOf("scene.bpm")),
      length_(params.idOf("pattern.length")),
      mutes_(params.idOf("pattern.mutes")),
      solos_(params.idOf("pattern.solos")),
      quantize_(params.idOf("project.patternQuantize"))
{
}

std::uint16_t SceneSync::muteMask(const m::Pattern& p) noexcept
{
    std::uint16_t mask = 0;
    for (int t = 0; t < m::kTracks; ++t)
        if (p.trackMutes[static_cast<std::size_t>(t)])
            mask = static_cast<std::uint16_t>(mask | 1u << t);
    return mask;
}

std::uint16_t SceneSync::soloMask(const m::Pattern& p) noexcept
{
    std::uint16_t mask = 0;
    for (int t = 0; t < m::kTracks; ++t)
        if (p.trackSolos[static_cast<std::size_t>(t)])
            mask = static_cast<std::uint16_t>(mask | 1u << t);
    return mask;
}

int SceneSync::quantizeIndex(const m::Project& p) noexcept
{
    switch (p.settings.patternQuantize)
    {
        case m::QuantizeUnit::Bar: return 1;
        case m::QuantizeUnit::Pattern: return 2;
        case m::QuantizeUnit::Off:
        case m::QuantizeUnit::Step:
        case m::QuantizeUnit::Beat: break; // no OFF / BEAT unit in the engine yet: the nearest finer one
    }
    return 0;
}

void SceneSync::post(const std::optional<core::ParamId>& id, std::uint16_t scope, float value)
{
    if (id && bridge_.post(engine::ParamChange {*id, scope, value, 0}))
        ++posted_;
}

void SceneSync::postScene(const m::Project& project, int scene)
{
    const auto& s = project.scenes[static_cast<std::size_t>(scene)];
    post(bpm_, static_cast<std::uint16_t>(scene), s.bpm ? static_cast<float>(*s.bpm) : 0.0f);
    for (int p = 0; p < m::kPatterns; ++p)
        postPattern(project, scene, p);
}

void SceneSync::postPattern(const m::Project& project, int scene, int pattern)
{
    const auto& p = project.scenes[static_cast<std::size_t>(scene)].patterns[static_cast<std::size_t>(pattern)];
    const auto scope = static_cast<std::uint16_t>(scene << 4 | pattern);
    post(length_, scope, static_cast<float>(p.length));
    post(mutes_, scope, static_cast<float>(muteMask(p)));
    post(solos_, scope, static_cast<float>(soloMask(p)));
}

void SceneSync::onChange(const core::ChangeSet& changes, const m::Project& project)
{
    for (const auto& path : changes.paths())
    {
        std::string_view s = path;
        if (const auto v = core::commands::scene::queueValueOf(s))
        {
            post(queue_, 0xFFFF, static_cast<float>(*v));
            continue;
        }
        if (s == "settings" || s == "settings.patternQuantize")
        {
            post(quantize_, 0, static_cast<float>(quantizeIndex(project)));
            continue;
        }
        if (! startsWith(s, "scene"))
            continue;
        s.remove_prefix(5);
        int scene = 0;
        if (! index(s, scene))
            continue;
        if (s.empty())
            postScene(project, scene);
        else if (s == ".bpm")
            post(bpm_, static_cast<std::uint16_t>(scene),
                 project.scenes[static_cast<std::size_t>(scene)].bpm
                     ? static_cast<float>(*project.scenes[static_cast<std::size_t>(scene)].bpm)
                     : 0.0f);
        else if (startsWith(s, ".pattern"))
        {
            s.remove_prefix(8);
            int pattern = 0;
            if (index(s, pattern))
                postPattern(project, scene, pattern); // length, mutes or solos (or the whole pattern)
        }
    }
}

void SceneSync::syncAll(const m::Project& project)
{
    for (int s = 0; s < m::kScenes; ++s)
        postScene(project, s);
    post(quantize_, 0, static_cast<float>(quantizeIndex(project)));
    const int scene = project.settings.currentScene & 15, pattern = project.settings.currentPattern & 15;
    post(queue_, 0xFFFF, static_cast<float>(scene * 16 + pattern));
}

void SceneSync::setSwitchable(bool on)
{
    post(queue_, 0xFFFF,
         static_cast<float>(on ? core::commands::scene::kQueueUnblock : core::commands::scene::kQueueBlock));
}
} // namespace ui::input::scenes
