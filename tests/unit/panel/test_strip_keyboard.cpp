// The touch strip's CDC frames (emulator panel_map.py E5) and the host keyboard map (control-surface.md UI-120…122).
#include "platform/host/input/KeyMap.h"
#include "platform/host/input/KeyboardSource.h"
#include "platform/imx6/strip/StripCodec.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using Kind = io::ControlEvent::Kind;
using namespace platform::host;

TEST_CASE("panel strip: E5 decoding and Touch / Value / Release events")
{
    using platform::imx6::strip::decodeFrame;
    // The emulator's frames: contact = (0x7f - position, 0x80, 0, 0), idle = (0x7f - 64, 0, 0, 0).
    const std::uint8_t idle[] = {0x7f - 64, 0, 0, 0};
    auto touched = [](int position) { return std::array<std::uint8_t, 4> {std::uint8_t(0x7f - position), 0x80, 0, 0}; };
    CHECK(decodeFrame(idle).contact == false);
    CHECK(decodeFrame(idle).position == 64);
    CHECK(decodeFrame(touched(0).data()).position == 0);
    CHECK(decodeFrame(touched(127).data()).position == 127);
    CHECK(decodeFrame(touched(11).data()).contact);

    platform::imx6::strip::StripDecoder d;
    io::ControlEvent e {};
    CHECK_FALSE(d.decode(idle, 0, e));
    // `./sp16 ctl event strip value 90` → position round(90 · 127 / 1000) = 11
    REQUIRE(d.decode(touched(11).data(), 1, e));
    CHECK(e.kind == Kind::Touch);
    CHECK(e.control == io::Control::Strip);
    CHECK(e.value == 11);
    CHECK(e.source == io::kSourceStrip);
    CHECK_FALSE(d.decode(touched(11).data(), 2, e));
    REQUIRE(d.decode(touched(100).data(), 3, e));
    CHECK(e.kind == Kind::Value);
    CHECK(e.value == 100);
    REQUIRE(d.decode(idle, 4, e));
    CHECK(e.kind == Kind::Release);
    CHECK(e.value == 100);
    CHECK_FALSE(d.decode(idle, 5, e));
}

namespace
{
struct Sink final : engine::TriggerSink
{
    std::vector<engine::TriggerNow> hits;
    bool push(const engine::TriggerNow& t) noexcept override
    {
        hits.push_back(t);
        return true;
    }
};

std::vector<io::ControlEvent> tap(KeyMap& m, int code, unsigned mods = 0, engine::TriggerSink* sink = nullptr)
{
    std::vector<io::ControlEvent> out;
    m.key(code, mods, true, false, 0, sink, out);
    m.key(code, mods, false, false, 0, sink, out);
    return out;
}
} // namespace

TEST_CASE("controls keyboard: the UI-120 default map")
{
    struct Case
    {
        int code;
        unsigned mods;
        io::Control control;
    };
    const Case cases[] = {
        {'1', 0, io::pad(13)}, {'4', 0, io::pad(16)}, {'q', 0, io::pad(9)}, {'r', 0, io::pad(12)},
        {'a', 0, io::pad(5)}, {'f', 0, io::pad(8)}, {'z', 0, io::pad(1)}, {'v', 0, io::pad(4)},
        {'5', 0, io::step(1)}, {'=', 0, io::step(8)}, {'t', 0, io::step(9)}, {']', 0, io::step(16)},
        {kKeyF1, 0, io::Control::Bar1}, {kKeyF1 + 3, 0, io::Control::Bar4}, {kKeyF1 + 4, 0, io::Control::PadModeTrack},
        {kKeyF1 + 7, 0, io::Control::PadModeScale}, {kKeyF1 + 8, 0, io::Control::Scene},
        {kKeyF1 + 9, 0, io::Control::Pattern}, {kKeyF1 + 10, 0, io::Control::StripMode},
        {kKeyF1 + 11, 0, io::Control::StripHold}, {' ', 0, io::Control::Play}, {'b', 0, io::Control::Stop},
        {'g', 0, io::Control::Rec}, {kKeyReturn, 0, io::Control::SelectorPush}, {kKeyEscape, 0, io::Control::Back},
        {kKeyHome, 0, io::Control::Home}, {'`', 0, io::Control::Home}, {'c', kModCmd, io::Control::ExtCopy},
        {'v', kModCmd, io::Control::ExtPaste}, {'z', kModCmd, io::Control::ExtUndo},
        {kKeyDelete, 0, io::Control::ExtClear}, {'n', 0, io::Control::ExtTap}, {'m', 0, io::Control::ExtCue},
        {'j', 0, io::Control::ExtErase}, {'k', 0, io::Control::ExtAccent}, {'1', kModCmd, io::Control::ExtMixer},
        {'4', kModCmd, io::Control::ExtProject}, {'2', kModCmd, io::Control::ExtBrowse},
        {'3', kModCmd, io::Control::ExtUtility},
        // Our choice: NUDGE has no UI-120 key; `;` / `'` (also their shifted forms) hold it
        {';', 0, io::Control::ExtNudgeDown}, {'\'', 0, io::Control::ExtNudgeUp}, {':', 0, io::Control::ExtNudgeDown},
    };
    for (const auto& c : cases)
    {
        INFO("key " << c.code << " mods " << c.mods << " → " << io::nameOf(c.control));
        KeyMap m;
        const auto ev = tap(m, c.code, c.mods);
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].kind == Kind::Press);
        CHECK(ev[0].control == c.control);
        CHECK(ev[0].source == io::kSourceKeyboard);
        CHECK(ev[1].kind == Kind::Release);
        CHECK(ev[1].control == c.control);
    }
    KeyMap m;
    const auto redo = tap(m, 'z', kModCmd | kModShift);
    REQUIRE(redo.size() == 3); // shift press, redo press/release (shift stays held)
    CHECK(redo[0].control == io::Control::Shift);
    CHECK(redo[1].control == io::Control::ExtRedo);
}

TEST_CASE("controls keyboard: pads trigger the engine directly at 100, accent 127, repeats ignored")
{
    Sink sink;
    KeyMap m;
    auto ev = tap(m, 'x', 0, &sink);
    REQUIRE(sink.hits.size() == 1);
    CHECK(sink.hits[0].track == 1);
    CHECK(sink.hits[0].velocity == 100);
    CHECK(ev[0].value == 100);

    std::vector<io::ControlEvent> out;
    m.key('k', 0, true, false, 0, &sink, out); // accent held
    m.key('x', 0, true, false, 0, &sink, out);
    m.key('x', 0, true, true, 0, &sink, out); // auto-repeat
    m.key('x', 0, false, false, 0, &sink, out);
    m.key('k', 0, false, false, 0, &sink, out);
    REQUIRE(sink.hits.size() == 2);
    CHECK(sink.hits[1].velocity == 127);
    REQUIRE(out.size() == 4);
    CHECK(out[1].value == 127);

    m.setFixedVelocity(64);
    tap(m, 'x', 0, &sink);
    CHECK(sink.hits.back().velocity == 64);
}

TEST_CASE("controls keyboard: encoders, column focus, selector, fine, shift and text mode")
{
    KeyMap m;
    std::vector<io::ControlEvent> out;
    m.key(kKeyUp, 0, true, false, 0, nullptr, out);
    m.key(kKeyUp, 0, true, true, 0, nullptr, out); // encoders repeat
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == Kind::Turn);
    CHECK(out[0].control == io::knob(1));
    CHECK(out[1].value == 1);
    out.clear();
    m.key(kKeyRight, 0, true, false, 0, nullptr, out);
    m.key(kKeyRight, 0, true, false, 0, nullptr, out);
    m.key(kKeyDown, 0, true, false, 0, nullptr, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].control == io::knob(3));
    CHECK(out[0].value == -1);
    out.clear();
    m.key('6', kModCtrl, true, false, 0, nullptr, out);
    CHECK(m.focusedColumn() == 6);
    m.key(kKeyRight, 0, true, false, 0, nullptr, out);
    CHECK(m.focusedColumn() == 6);
    m.key(kKeyUp, kModAlt, true, false, 0, nullptr, out); // fine = SHIFT + knob
    REQUIRE(out.size() == 3);
    CHECK((out[0].kind == Kind::Press && out[0].control == io::Control::Shift));
    CHECK((out[1].kind == Kind::Turn && out[1].control == io::knob(6)));
    CHECK((out[2].kind == Kind::Release && out[2].control == io::Control::Shift));
    out.clear();
    m.key('.', 0, true, false, 0, nullptr, out);
    m.key(',', 0, true, false, 0, nullptr, out);
    m.key('<', kModShift, true, false, 0, nullptr, out); // Shift + ',' = SHIFT + sel turn
    REQUIRE(out.size() == 4);
    CHECK((out[0].control == io::Control::Selector && out[0].value == 1));
    CHECK((out[1].control == io::Control::Selector && out[1].value == -1));
    CHECK((out[2].kind == Kind::Press && out[2].control == io::Control::Shift));
    CHECK((out[3].control == io::Control::Selector && out[3].value == -1));
    out.clear();
    m.modifiers(0, 0, out);
    REQUIRE(out.size() == 1);
    CHECK((out[0].kind == Kind::Release && out[0].control == io::Control::Shift));

    out.clear();
    m.setTextMode(true); // UI-122
    m.key('a', 0, true, false, 0, nullptr, out);
    m.key(' ', 0, true, false, 0, nullptr, out);
    m.key(kKeyEscape, 0, true, false, 0, nullptr, out);
    m.key(kKeyEscape, 0, false, false, 0, nullptr, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].control == io::Control::Back);
}

TEST_CASE("controls keyboard: KeyboardSource queues events for poll() and maps JUCE keys")
{
    Sink sink;
    KeyboardSource source(&sink);
    std::string info;
    CHECK(source.start(info));
    CHECK(source.id() == "keyboard");
    CHECK(source.key(' ', 0, true, false, 1));
    CHECK(source.key(' ', 0, false, false, 2));
    std::vector<io::ControlEvent> out;
    source.poll(out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].control == io::Control::Play);
    out.clear();
    source.poll(out);
    CHECK(out.empty());
    CHECK(KeyboardSource::codeFor(juce::KeyPress(juce::KeyPress::F5Key)) == kKeyF1 + 4);
    CHECK(KeyboardSource::codeFor(juce::KeyPress(juce::KeyPress::escapeKey)) == kKeyEscape);
    CHECK(KeyboardSource::codeFor(juce::KeyPress('q')) == 'q');
    CHECK(KeyboardSource::modsFor(juce::ModifierKeys(juce::ModifierKeys::shiftModifier)) == kModShift);
    CHECK(dynamic_cast<juce::KeyListener*>(static_cast<io::ControlSource*>(&source)) != nullptr);
}
