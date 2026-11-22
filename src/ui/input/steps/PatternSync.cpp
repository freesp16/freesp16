#include "ui/input/steps/PatternSync.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/Project.h"
#include "core/util/sequencer/PatternImage.h"

#include <array>
#include <bitset>
#include <charconv>
#include <memory>
#include <string_view>

namespace ui::input::steps
{
namespace seqcmd = core::commands::sequencer;

bool postPattern(engine::EngineBridge& bridge, const core::model::Project& project, int scene, int pattern)
{
    const auto compiler = core::sequencer::patternCompiler();
    if (compiler.build == nullptr)
        return false;
    auto image = std::make_unique<core::sequencer::PatternImage>(); // ≈ 8 KB: heap, not the stack
    const auto& s = project.scenes[static_cast<std::size_t>(scene & 15)];
    seqcmd::makePatternImage(s.patterns[static_cast<std::size_t>(pattern & 15)], *image);
    const void* object = compiler.build(*image);
    if (object == nullptr)
        return false;
    const engine::SwapSlot slot {engine::SwapSlot::Pattern, static_cast<std::uint8_t>(scene & 15),
                                 static_cast<std::uint8_t>(pattern & 15)};
    if (bridge.post(engine::ObjectSwap {slot, object}))
        return true;
    if (compiler.destroy != nullptr)
        compiler.destroy(object); // refused: ownership stayed here
    return false;
}

namespace
{
// "scene[s]" alone or "scene[s].track[t]…" (TrackSet copies the scene's patterns too): the scene's 16 patterns.
bool parseScene(std::string_view path, int& scene) noexcept
{
    constexpr std::string_view kScene = "scene[";
    if (path.substr(0, kScene.size()) != kScene)
        return false;
    const char* end = path.data() + path.size();
    int s = 0;
    const auto r = std::from_chars(path.data() + kScene.size(), end, s);
    if (r.ec != std::errc {} || r.ptr == end || *r.ptr != ']' || s < 0 || s > 15)
        return false;
    const std::string_view rest(r.ptr + 1, static_cast<std::size_t>(end - r.ptr - 1));
    if (! rest.empty() && rest.substr(0, 7) != ".track[")
        return false;
    if (! rest.empty() && rest.find('.', 1) != std::string_view::npos)
        return false; // a track field ("scene[0].track[3].volume"): no pattern changed
    scene = s;
    return true;
}
} // namespace

void forwardPatternChanges(const core::ChangeSet& changes, const core::model::Project& project,
                           engine::EngineBridge& bridge)
{
    std::bitset<256> dirty;
    for (const auto& path : changes.paths())
    {
        int s = 0, p = 0;
        if (seqcmd::parsePatternPath(path, s, p))
            dirty.set(static_cast<std::size_t>(s * 16 + p));
        else if (parseScene(path, s))
            for (int q = 0; q < 16; ++q)
                dirty.set(static_cast<std::size_t>(s * 16 + q));
    }
    for (std::size_t i = 0; i < dirty.size(); ++i)
        if (dirty.test(i))
            postPattern(bridge, project, static_cast<int>(i / 16), static_cast<int>(i % 16));
}

void forwardAllSequencer(const core::model::Project& project, engine::EngineBridge& bridge,
                         const core::ParamRegistry& params)
{
    for (int s = 0; s < 16; ++s)
        for (int p = 0; p < 16; ++p)
            postPattern(bridge, project, s, p);
    const auto post = [&](std::string_view id, std::uint16_t scope, float v) {
        if (const auto pid = params.idOf(id))
            bridge.post(engine::ParamChange {*pid, scope, v, 0});
    };
    post("seq.swing", 0, swingShownFromStored(project.settings.swing));
    post("seq.padStartsSequence", 0, project.settings.padStartsSequence ? 1.0f : 0.0f);
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
            post("seq.offset", static_cast<std::uint16_t>(s << 4 | t),
                 static_cast<float>(project.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)].seq.offset));
}
} // namespace ui::input::steps
