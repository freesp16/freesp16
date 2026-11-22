// The strip's input side: the CDC frame decode table into the engine's strip stream, the message thread's
// io::StripRouter (MODE / HOLD, re-posting, pressure, settings, lamps), the scripted `strip` command and the linux
// window's Ctrl + wheel stand-in. Synthetic frames from the documented layout only (never captured traffic).
#include "tests/unit/ribbon/RibbonTestUtil.h"

#include "core/commands/Command.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "io/controllers/ScriptedInput.h"
#include "io/controllers/StripSource.h"
#include "platform/imx6/strip/StripCodec.h"
#include "platform/linux/input/StripWheel.h"
#include "tests/fakes/FakeEngineBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <variant>

using Kind = io::ControlEvent::Kind;
using core::ribbon::Mode;

namespace
{
std::array<std::uint8_t, 4> frame(int b0, bool contact) { return {std::uint8_t(b0), std::uint8_t(contact ? 0x80 : 0), 0, 0}; }

std::vector<engine::ParamChange> params(const fakes::FakeEngineBridge& b, std::string_view name)
{
    std::vector<engine::ParamChange> out;
    for (const auto& m : b.posted)
        if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == ribbontest::id(name))
            out.push_back(*c);
    return out;
}
std::vector<engine::TriggerNow> triggers(const fakes::FakeEngineBridge& b)
{
    std::vector<engine::TriggerNow> out;
    for (const auto& m : b.posted)
        if (const auto* t = std::get_if<engine::TriggerNow>(&m))
            out.push_back(*t);
    return out;
}
io::ControlEvent ev(Kind k, io::Control c, int v, std::uint8_t source = io::kSourceKeyboard)
{
    return {k, c, static_cast<std::int16_t>(v), source, 0.0};
}
} // namespace

TEST_CASE("ribbon.frames: the decode table b0 = 0…0x7f, contact on / off, into the engine's strip stream", "[ribbon]")
{
    using platform::imx6::strip::decodeFrame;
    for (int b0 = 0; b0 <= 0x7f; ++b0)
        for (bool contact : {false, true})
        {
            INFO("b0 " << b0 << " contact " << contact);
            const auto f = frame(b0, contact);
            const auto s = decodeFrame(f.data());
            CHECK(s.position == 0x7f - b0); // value = (0x7f − b0) | b1 << 8, position = value & 0x7f (0 bottom)
            CHECK(s.contact == contact);    // bit 15
            // bytes 2–3 are ignored (not documented)
            auto g = f;
            g[2] = 0x5a;
            g[3] = 0xa5;
            CHECK(decodeFrame(g.data()).position == s.position);
            CHECK(decodeFrame(g.data()).contact == s.contact);
        }
    // Frames → reader events → the strip stream → the engine's ribbon (the device path, minus the thread).
    platform::imx6::strip::StripDecoder d;
    auto r = ribbontest::makeRibbon();
    engine::trigger::PadState pads;
    int events = 0;
    for (int b0 = 0x7f; b0 >= 0; b0 -= 3) // a sweep bottom → top
    {
        io::ControlEvent e {};
        const auto f = frame(b0, true);
        if (! d.decode(f.data(), 0, e))
            continue;
        ++events;
        CHECK(e.kind == (events == 1 ? Kind::Touch : Kind::Value));
        REQUIRE(r.onLive(io::stripTrigger(e.value, e.kind != Kind::Release), pads));
        CHECK(r.position() == 0x7f - b0);
        CHECK(r.contact());
        CHECK(r.ledMask() == core::ribbon::ledMask(Mode::Pitch, 0x7f - b0, true)); // LEDs follow the position
    }
    io::ControlEvent e {};
    const auto lift = frame(0x7f - 64, false);
    REQUIRE(d.decode(lift.data(), 0, e));
    CHECK(e.kind == Kind::Release);
    REQUIRE(r.onLive(io::stripTrigger(e.value, false), pads));
    CHECK_FALSE(r.contact());
    CHECK(r.ledMask() == 0);
}

TEST_CASE("ribbon.router: MODE cycles PITCH → REPEAT → USER1 → USER2, cancels HOLD; HOLD toggles", "[ribbon]")
{
    fakes::FakeEngineBridge b;
    io::StripRouter s(b, core::builtinParams());
    std::vector<std::string> lines;
    s.log = [&](const std::string& l) { lines.push_back(l); };
    CHECK(s.mode() == Mode::Pitch);
    const Mode order[] = {Mode::Repeat, Mode::User1, Mode::User2, Mode::Pitch};
    for (const auto m : order)
    {
        s.handle(ev(Kind::Press, io::Control::StripMode, 100));
        s.handle(ev(Kind::Release, io::Control::StripMode, 0)); // only the press counts
        CHECK(s.mode() == m);
    }
    const auto modes = params(b, "ribbon.mode");
    REQUIRE(modes.size() == 4);
    CHECK(modes[0].value == 1.0f);
    CHECK(modes[3].value == 0.0f);
    s.handle(ev(Kind::Press, io::Control::StripHold, 100));
    CHECK(s.hold());
    CHECK(params(b, "ribbon.hold").back().value == 1.0f);
    s.handle(ev(Kind::Press, io::Control::StripMode, 100)); // RIB-005
    CHECK_FALSE(s.hold());
    CHECK(params(b, "ribbon.hold").back().value == 0.0f);
    CHECK(lines.back().rfind("RIBBON mode mode=REPEAT hold=0", 0) == 0);

    io::LampFrame f;
    s.lamps(f);
    CHECK(f[io::Control::StripModeRepeat].mode == io::LampMode::On);
    CHECK(f[io::Control::StripModePitch].mode == io::LampMode::Off);
    CHECK(f[io::Control::StripHold].mode == io::LampMode::Off);
}

TEST_CASE("ribbon.router: strips from other sources are posted, the device strip is not; LEDs and HOLD", "[ribbon]")
{
    fakes::FakeEngineBridge b;
    io::StripRouter s(b, core::builtinParams());
    s.handle(ev(Kind::Touch, io::Control::Strip, 20, io::kSourceStrip)); // the reader thread already sent it
    s.handle(ev(Kind::Value, io::Control::Strip, 30, io::kSourceStrip));
    CHECK(triggers(b).empty());
    CHECK(s.contact());
    CHECK(s.position() == 30);
    io::LampFrame f;
    s.lamps(f);
    for (int i = 0; i < 8; ++i) // PITCH: lit up to segment 30 / 16 = 1
        CHECK((f[static_cast<io::Control>(static_cast<int>(io::Control::StripLed1) + i)].mode == io::LampMode::On) == (i <= 1));
    s.handle(ev(Kind::Release, io::Control::Strip, 30, io::kSourceStrip));
    CHECK(s.ledMask() == 0);

    s.handle(ev(Kind::Value, io::Control::Strip, 127, io::kSourceVirtual)); // a script's first value lands the finger
    s.handle(ev(Kind::Press, io::Control::StripHold, 100));
    s.handle(ev(Kind::Release, io::Control::Strip, 0, io::kSourceVirtual));
    const auto t = triggers(b);
    REQUIRE(t.size() == 2);
    CHECK(t[0].pad == core::ribbon::kStripPad);
    CHECK(t[0].velocity == 127);
    CHECK(t[0].note == 1);
    CHECK(t[1].velocity == 127); // the release keeps the last position
    CHECK(t[1].note == 0);
    CHECK(s.shownPosition() == 127); // HOLD keeps the LED (RIB-005)
    CHECK(s.ledMask() == 0xFF);
    s.lamps(f);
    CHECK(f[io::Control::StripHold].mode == io::LampMode::On);
    s.handle(ev(Kind::Press, io::Control::StripHold, 100));
    CHECK(s.ledMask() == 0);

    // pad pressure goes to the engine in REPEAT mode only
    s.handle(ev(Kind::Pressure, io::pad(3), 77, io::kSourcePanel));
    CHECK(triggers(b).size() == 2);
    s.handle(ev(Kind::Press, io::Control::StripMode, 100));
    s.handle(ev(Kind::Pressure, io::pad(3), 77, io::kSourcePanel));
    const auto p = triggers(b);
    REQUIRE(p.size() == 3);
    CHECK(p[2].pad == core::ribbon::kPressurePad);
    CHECK(p[2].track == 2);
    CHECK(p[2].velocity == 77);
}

TEST_CASE("ribbon.router: the project's ribbon settings become engine messages (RIB-009)", "[ribbon]")
{
    fakes::FakeEngineBridge b;
    io::StripRouter s(b, core::builtinParams());
    auto p = core::model::newDefaultProject();
    p->ribbon.user[1][2] = {"track.pan", 0.9, 0.1};
    p->ribbon.user[0][0] = {"no.such.param", 1.0, 0.0};
    core::ChangeSet c;
    c.add("ribbon");
    s.forwardChanges(c, *p);
    const auto targets = params(b, "ribbon.userTarget");
    REQUIRE(targets.size() == 16);
    CHECK(targets[10].scope == 10); // USER2 item 3 = set 1 × 8 + 2
    CHECK(targets[10].value == static_cast<float>(ribbontest::id("track.pan").value));
    CHECK(targets[0].value == -1.0f); // an unknown id is an empty slot
    CHECK(params(b, "ribbon.userTop")[10].value == 0.9f);
    CHECK(params(b, "ribbon.userBottom")[10].value == 0.1f);
    CHECK(params(b, "ribbon.pitchTop").back().value == 200.0f);
    CHECK(params(b, "ribbon.pitchBottom").back().value == -200.0f);
    const auto n = b.posted.size();
    core::ChangeSet other;
    other.add("scene[0].track[0].volume");
    s.forwardChanges(other, *p);
    CHECK(b.posted.size() == n);
}

TEST_CASE("ribbon.script: `strip <pos>` and `strip off`", "[ribbon]")
{
    const auto r = io::parseInputScript("100 strip 64\n200 strip 300\n300 strip off\n400 strip\n500 strip x\n");
    REQUIRE(r.steps.size() == 3);
    CHECK(r.errors.size() == 2);
    CHECK(r.steps[0].event.control == io::Control::Strip);
    CHECK(r.steps[0].event.kind == Kind::Value);
    CHECK(r.steps[0].event.value == 64);
    CHECK(r.steps[1].event.value == 127);
    CHECK(r.steps[2].event.kind == Kind::Release);
}

TEST_CASE("ribbon.desktop: Ctrl + wheel touches, moves and (Ctrl up) lifts the strip", "[ribbon]")
{
    platform::linux_desktop::StripWheel w;
    CHECK_FALSE(w.lift(0).has_value());
    auto e = w.wheel(+1, 1);
    CHECK(e.kind == Kind::Touch);
    CHECK(e.control == io::Control::Strip);
    CHECK(e.value == 72);
    CHECK(e.source == io::kSourceKeyboard);
    for (int i = 0; i < 20; ++i)
        e = w.wheel(+1, 2);
    CHECK(e.kind == Kind::Value);
    CHECK(e.value == 127);
    for (int i = 0; i < 20; ++i)
        e = w.wheel(-1, 3);
    CHECK(e.value == 0);
    const auto up = w.lift(4);
    REQUIRE(up.has_value());
    CHECK(up->kind == Kind::Release);
    CHECK(up->value == 0);
    CHECK(w.wheel(+1, 5).kind == Kind::Touch); // the next touch starts from the last position
    CHECK(w.position() == 8);
}
