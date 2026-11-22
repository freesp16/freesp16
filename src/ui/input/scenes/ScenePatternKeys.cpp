#include "ui/input/scenes/ScenePatternKeys.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/model/Project.h"
#include "ui/theme/Theme.h"

#include <algorithm>

namespace ui::input::scenes
{
namespace sc = core::commands::scene;

namespace
{
constexpr int kMinLength = 8, kMaxLength = 64; // SCN-011: the UI's range (files may hold 1–64)

bool isBar(io::Control c) noexcept
{
    return c >= io::Control::Bar1 && c <= io::Control::Bar4;
}
} // namespace

ScenePatternKeys::ScenePatternKeys(core::CommandBus& bus) : bus_(bus)
{
    changes_ = bus_.observe("scene", [this](const core::ChangeSet&) { contentDirty_ = true; });
}

ScenePatternKeys::~ScenePatternKeys() = default;

const core::model::Project* ScenePatternKeys::project() const noexcept
{
    return core::commands::hasProject(bus_) ? &core::commands::project(bus_) : nullptr;
}

int ScenePatternKeys::currentScene() const noexcept
{
    if (feed_.blockIndex > 0)
        return feed_.scene & 15;
    const auto* p = project();
    return p != nullptr ? p->settings.currentScene & 15 : 0;
}

int ScenePatternKeys::currentPattern() const noexcept
{
    if (feed_.blockIndex > 0)
        return feed_.pattern & 15;
    const auto* p = project();
    return p != nullptr ? p->settings.currentPattern & 15 : 0;
}

void ScenePatternKeys::onScene()
{
    chain_.cancel();
    mode_ = mode_ == KeyMode::Scene || mode_ == KeyMode::ScenePatterns ? KeyMode::Off : KeyMode::Scene;
}

void ScenePatternKeys::onPattern(bool shift)
{
    chain_.cancel(); // leaving / switching mode while holding: no chain, no selection
    chainKey_ = 0;
    if (shift)
        mode_ = mode_ == KeyMode::Length ? KeyMode::Off : KeyMode::Length;
    else
        mode_ = mode_ == KeyMode::Pattern || mode_ == KeyMode::Length ? KeyMode::Off : KeyMode::Pattern;
}

void ScenePatternKeys::onStepKey(int key)
{
    if (key < 1 || key > 16)
        return;
    switch (mode_)
    {
    case KeyMode::Pattern:
        if (chain_.holding()) // SCN-030: the current key is held: build the chain
        {
            chain_.add(key - 1);
            break;
        }
        if (chainSink_ && key - 1 == currentPattern())
        {
            chain_.begin(currentScene(), key - 1); // decided on its release: a chain, or a plain cancel
            chainKey_ = key;
            break;
        }
        clearChain();
        select(currentScene(), key - 1);
        break;
    case KeyMode::Scene:
        chosenScene_ = key - 1; // stage 2: the keys show that scene's patterns
        mode_ = KeyMode::ScenePatterns;
        break;
    case KeyMode::ScenePatterns:
        clearChain();
        select(chosenScene_, key - 1);
        break;
    case KeyMode::Length:
    {
        const auto* p = project();
        const int length = p != nullptr ? p->scenes[static_cast<std::size_t>(currentScene())]
                                              .patterns[static_cast<std::size_t>(currentPattern())]
                                              .length
                                        : 16;
        const int bars = std::clamp((length + 15) / 16, 1, 4);
        setLength((bars - 1) * 16 + key);
        break;
    }
    case KeyMode::Off:
        break;
    }
}

void ScenePatternKeys::onBarKey(int bar)
{
    if (mode_ == KeyMode::Length && bar >= 1 && bar <= 4)
        setLength(bar * 16);
}

bool ScenePatternKeys::handle(const io::ControlEvent& e, bool shift)
{
    using Kind = io::ControlEvent::Kind;
    const bool scenePattern = e.control == io::Control::Scene || e.control == io::Control::Pattern;
    const bool step = io::isStep(e.control) && ownsStepKeys();
    const bool bar = isBar(e.control) && mode_ == KeyMode::Length;
    if (e.kind != Kind::Press)
    {
        if (e.kind == Kind::Release && step)
            onStepRelease(io::indexOf(e.control));
        return (e.kind == Kind::Release) && (scenePattern || step || bar);
    }
    if (e.control == io::Control::Scene)
        onScene();
    else if (e.control == io::Control::Pattern)
        onPattern(shift);
    else if (step)
        onStepKey(io::indexOf(e.control));
    else if (bar)
        onBarKey(static_cast<int>(e.control) - static_cast<int>(io::Control::Bar1) + 1);
    else
        return false;
    return true;
}

void ScenePatternKeys::observe(const engine::PlayheadFeed& feed) noexcept
{
    feed_ = feed;
}

void ScenePatternKeys::onEngineEvent(const engine::EngineEvent& e)
{
    if (e.kind != engine::EngineEvent::Kind::SwitchDone || e.value < 0)
        return;
    const int v = e.value & 255;
    bus_.execute(sc::setCurrentScenePattern(v / 16, v % 16));
}

void ScenePatternKeys::select(int scene, int pattern)
{
    const auto* p = project();
    if (policy_ == BpmPolicy::Ask && askBpm_ && p != nullptr && scene != currentScene())
        if (const auto& bpm = p->scenes[static_cast<std::size_t>(scene & 15)].bpm)
        {
            askBpm_(scene, *bpm, [this, scene, pattern](bool apply)
                    { bus_.execute(sc::selectScenePattern(scene, pattern, apply)); });
            return;
        }
    bus_.execute(sc::selectScenePattern(scene, pattern));
}

void ScenePatternKeys::onStepRelease(int key)
{
    if (! chain_.holding() || key != chainKey_)
        return;
    chainKey_ = 0;
    if (const auto plan = chain_.end())
    {
        chainSink_(*plan);
        chainPosted_ = true;
        return;
    }
    clearChain(); // the current key alone: SCN-021 cancel, as a plain press
    select(currentScene(), key - 1);
}

void ScenePatternKeys::clearChain()
{
    if (! chainPosted_ || ! chainSink_)
        return;
    chainPosted_ = false;
    chainSink_(core::edit::PatternChainBuilder::clear());
}

void ScenePatternKeys::setLength(int steps)
{
    const auto* p = project();
    const int n = std::clamp(steps, kMinLength, kMaxLength);
    bus_.execute(p != nullptr ? sc::resizePattern(*p, currentScene(), currentPattern(), n) // longer: repeats the bars
                              : sc::setPatternLength(currentScene(), currentPattern(), n));
}

void ScenePatternKeys::refreshContent() const noexcept
{
    if (! contentDirty_)
        return;
    contentDirty_ = false;
    sceneContent_ = 0;
    patternContent_.fill(0);
    const auto* p = project();
    if (p == nullptr)
        return;
    for (int s = 0; s < 16; ++s)
    {
        const auto& scene = p->scenes[static_cast<std::size_t>(s)];
        const auto mask = sc::patternContentMask(scene);
        patternContent_[static_cast<std::size_t>(s)] = mask;
        if (mask != 0 || ! scene.name.empty() || scene.bpm.has_value())
            sceneContent_ = static_cast<std::uint16_t>(sceneContent_ | 1u << s);
    }
}

void ScenePatternKeys::lamps(io::LampFrame& f, const engine::PlayheadFeed& feed, double nowMs) const noexcept
{
    (void) nowMs; // blinks run from time 0 (2 Hz, UI-150), like the SCENE MANAGER's pending key
    if (mode_ == KeyMode::Off)
        return;
    refreshContent();
    const auto* p = project();
    const bool live = feed.blockIndex > 0;
    const int curScene = live ? feed.scene & 15 : (p != nullptr ? p->settings.currentScene & 15 : 0);
    const int curPattern = live ? feed.pattern & 15 : (p != nullptr ? p->settings.currentPattern & 15 : 0);
    const int pendScene = live && feed.pendingPattern >= 0 ? (feed.pendingScene >= 0 ? feed.pendingScene : curScene) : -1;
    const int pendPattern = live ? feed.pendingPattern : -1;
    const auto colour = [p](int scene) -> std::uint32_t
    {
        const int c = p != nullptr ? p->scenes[static_cast<std::size_t>(scene & 15)].colour : scene;
        return theme::sceneColour(c + 1) & 0x00FFFFFFu;
    };

    const bool sceneMode = mode_ == KeyMode::Scene || mode_ == KeyMode::ScenePatterns;
    f[io::Control::Scene] = {sceneMode ? io::LampMode::On : io::LampMode::Off, io::lampcolour::white, 0};
    f[io::Control::Pattern] = {sceneMode ? io::LampMode::Off : io::LampMode::On, io::lampcolour::white, 0};

    if (mode_ == KeyMode::Length)
    {
        const int length = p != nullptr
                               ? p->scenes[static_cast<std::size_t>(curScene)].patterns[static_cast<std::size_t>(curPattern)].length
                               : 16;
        io::lengthModeLamps(std::clamp((length + 15) / 16, 1, 4), f);
        return;
    }

    std::array<io::SlotState, 16> slots {};
    std::array<std::uint32_t, 16> rgb {};
    if (mode_ == KeyMode::Scene)
        for (int s = 0; s < 16; ++s)
        {
            const auto i = static_cast<std::size_t>(s);
            rgb[i] = colour(s);
            slots[i] = s == curScene                          ? io::SlotState::Current
                       : s == pendScene                       ? io::SlotState::Pending
                       : (sceneContent_ >> s & 1u) != 0       ? io::SlotState::Recorded
                                                              : io::SlotState::Empty;
        }
    else
    {
        const int shown = mode_ == KeyMode::ScenePatterns ? chosenScene_ : curScene;
        for (int q = 0; q < 16; ++q)
        {
            const auto i = static_cast<std::size_t>(q);
            rgb[i] = colour(shown);
            const bool content = (patternContent_[static_cast<std::size_t>(shown)] >> q & 1u) != 0;
            slots[i] = shown == curScene && q == curPattern          ? io::SlotState::Current
                       : shown == pendScene && q == pendPattern      ? io::SlotState::Pending
                       : content                                     ? io::SlotState::Recorded
                                                                     : io::SlotState::Empty;
        }
    }
    io::slotStepLamps(slots, rgb, 0.0, f);
    for (int k = 0; k < 16; ++k) // SCN-022: the pending key blinks at 2 Hz (the SCENE MANAGER's rate)
        if (slots[static_cast<std::size_t>(k)] == io::SlotState::Pending)
            f[io::step(k + 1)].mode = io::LampMode::BlinkSlow;
}

void ScenePatternKeys::paintLamps(io::LampSink& sink, const engine::PlayheadFeed& feed, double nowMs) const
{
    if (mode_ == KeyMode::Off)
        return;
    io::LampFrame f;
    lamps(f, feed, nowMs);
    const auto put = [&](io::Control c)
    {
        const auto out = io::resolve(f[c], nowMs);
        sink.setLamp(c, out.rgb, out.level);
    };
    put(io::Control::Scene);
    put(io::Control::Pattern);
    for (int k = 1; k <= 16; ++k)
        put(io::step(k));
    if (mode_ == KeyMode::Length)
        for (auto c : {io::Control::Bar1, io::Control::Bar2, io::Control::Bar3, io::Control::Bar4})
            put(c);
}
} // namespace ui::input::scenes
