// The performance screens (LFO, AMP ENVELOPE graph, SET SCALE, TOUCH STRIP SETTING) through the real shell with the
// built-in registries, the mockups' demo tracks, a fresh m2::Session (the document the TOUCH STRIP SETTING edits) and
// an FX environment with its own slots (FX destinations, stale targets).
#pragma once

#include "core/params/ParamRegistry.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/screens/sequence/M2Session.h"

#include <string>
#include <string_view>
#include <vector>

namespace ui
{
class LfoGraph;
const LfoGraph* lfoGraphOf(const Widget* body) noexcept; // ui/screens/lfo/LfoScreen.cpp
int touchStripPlace(const Widget* body) noexcept;        // ui/screens/touchstrip/TouchStripScreen.cpp
} // namespace ui

namespace m4test
{
struct M4Fixture
{
    ui::fx::LocalFxSlots slots;
    ui::fx::ScopedFxEnvironment env{[this] {
        ui::fx::FxEnvironment e;
        e.slots = &slots;
        return e;
    }()};
    m1test::M1Fixture f;

    explicit M4Fixture(bool logging = true) : f(true, logging) { ui::m2::Session::instance().reset(); }
    ~M4Fixture() { ui::m2::Session::instance().reset(); }

    ui::Shell& shell() { return *f.shell; }
    ui::UiState& state() { return f.shell->state(); }
    float get(std::string_view id, int track)
    {
        return ui::m1::trackParam(*f.shell, id, track);
    }
    void set(std::string_view id, int track, float v) { f.set(id, track, v); }
    float paramId(std::string_view id) { return static_cast<float>(core::builtinParams().idOf(id)->value); }
    bool open(std::string_view id)
    {
        const bool ok = f.shell->open(id);
        f.settle();
        return ok;
    }
    // Live transport: the feeds carry this playhead from the next frame on.
    void play(double ppq, bool playing = true, double bpm = 120)
    {
        engine::PlayheadFeed ph;
        ph.blockIndex = 1;
        ph.ppq = ppq;
        ph.bpm = bpm;
        ph.playing = playing;
        f.shell->feeds().inject(engine::MeterFeed{}, ph);
        f.shell->state().playing = playing;
    }
    void click(io::Control c)
    {
        f.press(c);
        f.release(c);
    }
    bool hasText(std::string_view text)
    {
        for (const auto& c : f.dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == text)
                return true;
        return false;
    }
    // Colour of the first text command equal to `text` (0 = none).
    std::uint32_t textColour(std::string_view text)
    {
        for (const auto& c : f.dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == text)
                return c.argb;
        return 0;
    }
};

// A mockup from resources/design/mockups/ (pinned) or tests/snapshot/m4/mockups/.
inline const snapshot::Image& m4mockup(std::string_view name)
{
    static const char* pinned[] = {"lfo-destination", "set-scale", "amp-envelope", "amp-envelope-setting"};
    for (const auto* p : pinned)
        if (name == p)
            return snapshot::mockup(name);
    static std::vector<std::string> keys;
    keys.reserve(32);
    keys.push_back("../../../tests/snapshot/m4/mockups/" + std::string(name));
    return snapshot::mockup(keys.back());
}
} // namespace m4test
