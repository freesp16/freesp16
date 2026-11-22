// The sampling and sync screens (SAMPLING/EDIT, LIVE SAMPLING, MIDI track, OUTPUT SETTING, ARRANGER, RENDER AUDIO)
// through the real shell with the built-in registries, the mockups' demo tracks and a fresh m2::Session (the document
// the ARRANGER and RENDER AUDIO read). Fakes for the sampling and render hosts live in the tests that need them.
#pragma once

#include "core/params/ParamRegistry.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/screens/sequence/M2Session.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ui
{
int arrangerFocus(const Widget* body) noexcept;    // ui/screens/arranger/ArrangerScreen.cpp
int arrangerFirstRow(const Widget* body) noexcept; // ui/screens/arranger/ArrangerScreen.cpp
void resetArranger() noexcept;
} // namespace ui

namespace m5test
{
struct M5Fixture
{
    m1test::M1Fixture f;

    explicit M5Fixture(bool logging = true) : f(true, logging)
    {
        ui::m2::Session::instance().reset();
        ui::resetArranger();
        auto& song = core::arrangement::liveSongStatus();
        song.row = -1;
        song.remaining = 0;
    }
    ~M5Fixture()
    {
        ui::m2::Session::instance().reset();
        ui::resetArranger();
        core::arrangement::liveSongStatus().row = -1;
    }

    ui::Shell& shell() { return *f.shell; }
    ui::UiState& state() { return f.shell->state(); }
    ui::m2::Session& session() { return ui::m2::Session::instance(); }
    const core::model::Project& project() { return session().project(); }
    float get(std::string_view id, int track = 1) { return ui::m1::trackParam(*f.shell, id, track); }
    void set(std::string_view id, int track, float v) { f.set(id, track, v); }
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
        f.shell->feeds().inject(engine::MeterFeed {}, ph);
        f.shell->state().playing = playing;
    }
    void click(io::Control c)
    {
        f.press(c);
        f.release(c);
    }
    void shiftClick(io::Control c)
    {
        f.press(io::Control::Shift);
        click(c);
        f.release(io::Control::Shift);
    }
    void key(io::Control c) { click(c); f.settle(1); }
    void turnSel(int d) { f.turn(io::Control::Selector, d); }
    void push() { click(io::Control::SelectorPush); }
    void knob(int n1, int d) { f.turn(io::knob(n1), d); }
    // The rotary onto rail item i (a body that leaves the rotary to the rail: the first turn focuses item 1).
    void railTo(int i)
    {
        if (f.shell->railFocus() < 0)
            turnSel(1);
        if (const int d = i - f.shell->railFocus(); d != 0)
            turnSel(d);
    }
    // NFR-040 / UI-420: a panel jump key (ext.mixer = MIXER, SHIFT + ext.mixer = ARRANGER, ext.project =
    // PROJECT; Shell::jumpKey).
    void jump(io::Control c, bool shift, std::string_view expect)
    {
        if (shift)
            f.press(io::Control::Shift);
        click(c);
        if (shift)
            f.release(io::Control::Shift);
        f.settle(1);
        REQUIRE(f.shell->current().id == expect);
    }
    bool hasText(std::string_view text)
    {
        for (const auto& c : f.dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == text)
                return true;
        return false;
    }
    std::uint32_t textColour(std::string_view text)
    {
        for (const auto& c : f.dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == text)
                return c.argb;
        return 0;
    }
    bool logHas(std::string_view needle) const
    {
        for (const auto& l : f.log)
            if (l.find(needle) != std::string::npos)
                return true;
        return false;
    }
};

// A mockup from resources/design/mockups/ (pinned) or tests/snapshot/m5/mockups/.
inline const snapshot::Image& m5mockup(std::string_view name)
{
    static const char* pinned[] = {"output-setting", "mixer-analog-filter", "mixer", "popups"};
    for (const auto* p : pinned)
        if (name == p)
            return snapshot::mockup(name);
    static std::vector<std::string> keys;
    keys.reserve(32);
    keys.push_back("../../../tests/snapshot/m5/mockups/" + std::string(name));
    return snapshot::mockup(keys.back());
}
} // namespace m5test
