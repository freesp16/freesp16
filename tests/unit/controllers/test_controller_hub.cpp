// The controller hub (control-surface.md UI-130, UI-136; screens.md UI-330): through the real MIDI ports
// (MidiService + the scripted virtual port, its reader thread and parser), messages a controller profile uses never
// reach the MIDI-track input nor the thru forwarder while the others do; lamps go out through the sender; roles and
// the per-device settings persist; a user profile made by MIDI learn survives a restart; the keyboard remap persists.
#include "io/controllers/profiles/ControllerHub.h"
#include "io/controllers/profiles/KeyRemap.h"
#include "io/midi/MidiService.h"
#include "io/midi/VirtualPort.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <thread>

namespace ctl = io::controllers;
using io::Control;
using Kind = io::ControlEvent::Kind;

namespace
{
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string tempDir()
{
    char tmpl[] = "/tmp/ctrl-hub-XXXXXX";
    const char* d = mkdtemp(tmpl);
    REQUIRE(d != nullptr);
    return d;
}

core::midi::MidiEvent ev(std::uint8_t a, std::uint8_t b, std::uint8_t c, int size = 3)
{
    core::midi::MidiEvent e;
    e.size = static_cast<std::uint8_t>(size);
    e.bytes[0] = a;
    e.bytes[1] = b;
    e.bytes[2] = c;
    return e;
}
} // namespace

TEST_CASE("consumed controller messages never reach the MIDI-track input, lamps go out (virtual port)",
          "[controllers]")
{
    core::midi::MidiHub hub;
    hub.setPortMode(0, 3); // THRU A on output 1: whatever is forwarded would be written back out
    auto script = io::midi::parseVirtualScript("100 99 24 64\n120 89 24 00\n140 B9 46 01\n160 90 3C 64\n"
                                               "180 B0 07 50\n200 99 25 50\n");
    REQUIRE(script.errors.empty());
    auto provider = std::make_unique<io::midi::VirtualProvider>(std::move(script));
    auto* virt = provider.get();
    std::vector<std::unique_ptr<io::midi::PortProvider>> providers;
    providers.push_back(std::move(provider));

    ctl::ControllerHub h;
    h.log = [](const std::string&) {};
    h.setOverride("VIRTUAL");
    h.install(); // before the ports open: told about them as they open
    io::midi::MidiService service(hub, std::move(providers));
    std::string info;
    service.start(info);
    REQUIRE(h.connectedCount() == 1);
    auto* src = h.source("VIRTUAL");
    REQUIRE(src != nullptr);
    CHECK(src->profileId() == ctl::kGenericProfileId);
    sleepMs(400);
    std::vector<io::ControlEvent> out;
    h.poll(out);
    REQUIRE(out.size() == 4);
    CHECK((out[0].kind == Kind::Press && out[0].control == io::pad(1) && out[0].value == 100));
    CHECK((out[1].kind == Kind::Release && out[1].control == io::pad(1)));
    CHECK((out[2].kind == Kind::Turn && out[2].control == io::knob(1)));
    CHECK((out[3].kind == Kind::Press && out[3].control == io::pad(2)));
    CHECK(out[0].source == io::kSourceMidi + 0);
    // Only the two channel-1 messages reached the engine's MIDI input.
    std::vector<core::midi::MidiEvent> in;
    for (core::midi::MidiEvent e; hub.popIn(0, e);)
        in.push_back(e);
    REQUIRE(in.size() == 2);
    CHECK(in[0].bytes[0] == 0x90);
    CHECK(in[1].bytes[0] == 0xB0);
    CHECK(service.messagesIn() == 6);

    // Lamps: the LampModel lights pad 1 in track 1's colour → a note-on through the sender on the same port's output.
    h.setLamp(io::pad(1), 0xFF7E72, 255);
    h.flush();
    sleepMs(50);
    const auto rec = virt->received();
    bool lampOn = false, forwardedPad = false;
    for (const auto& r : rec)
    {
        lampOn = lampOn || (r.bytes[0] == 0x99 && r.bytes[1] == 36 && r.bytes[2] == 1);
        forwardedPad = forwardedPad || (r.bytes[0] == 0x99 && r.bytes[2] == 0x64);
    }
    CHECK(lampOn);
    CHECK_FALSE(forwardedPad); // the consumed pad hit was not thru'd
    bool forwardedNote = false;
    for (const auto& r : rec)
        forwardedNote = forwardedNote || (r.bytes[0] == 0x90 && r.bytes[1] == 0x3C);
    CHECK(forwardedNote); // the instrument note was
    CHECK(service.sender().sentNow() >= 19);

    service.stop();
    CHECK(h.connectedCount() == 0);
    h.uninstall();
}

TEST_CASE("roles — AUTO by profile pattern, CONTROLLER / INSTRUMENT by hand, settings persist",
          "[controllers]")
{
    const std::string dir = tempDir();
    {
        ctl::ControllerHub h;
        h.log = [](const std::string&) {};
        h.setDataDir(dir);
        h.inputOpened(0, "Keys 49");
        CHECK(h.source("Keys 49") == nullptr); // AUTO: no profile names it → instrument
        CHECK_FALSE(h.message(0, ev(0x99, 36, 100)));
        h.setRole("Keys 49", ctl::PortRole::Controller);
        REQUIRE(h.source("Keys 49") != nullptr);
        CHECK(h.message(0, ev(0x99, 36, 100)));
        h.setResponse("Keys 49", {io::PadCurveShape::Soft2, 3, 9, 64});
        h.setTrim("Keys 49", 4, -7);
        h.setProfile("Keys 49", std::string(ctl::kReferenceProfileId));
        CHECK(h.source("Keys 49")->profileId() == ctl::kReferenceProfileId);
        CHECK(h.connectedCount() == 1);
        h.inputClosed(0);
    }
    ctl::ControllerHub again;
    again.log = [](const std::string&) {};
    again.setDataDir(dir);
    const auto ports = again.ports();
    REQUIRE(ports.size() == 1);
    CHECK(ports[0].name == "Keys 49");
    CHECK_FALSE(ports[0].connected);
    CHECK(ports[0].settings.role == ctl::PortRole::Controller);
    CHECK(ports[0].settings.profile == ctl::kReferenceProfileId);
    CHECK(ports[0].settings.response.shape == io::PadCurveShape::Soft2);
    CHECK(ports[0].settings.response.threshold == 3);
    CHECK(ports[0].settings.response.sensitivity == 9);
    CHECK(ports[0].settings.response.fixedVelocity == 64);
    CHECK(ports[0].settings.trim[4] == -7);
    again.inputOpened(1, "Keys 49");
    REQUIRE(again.source("Keys 49") != nullptr);
    CHECK(again.source("Keys 49")->profileId() == ctl::kReferenceProfileId);
    again.inputClosed(1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a user profile made by MIDI learn survives a restart", "[controllers]")
{
    const std::string dir = tempDir();
    {
        ctl::ControllerHub h;
        h.log = [](const std::string&) {};
        h.setDataDir(dir);
        h.inputOpened(0, "Odd Pads");
        h.setRole("Odd Pads", ctl::PortRole::Controller);
        REQUIRE(h.startLearn("Odd Pads"));
        CHECK(h.learnState() == ctl::LearnState::Waiting);
        CHECK(h.message(0, ev(0x92, 70, 100))); // the user hits a pad sending note 70 on channel 3
        std::vector<io::ControlEvent> out;
        h.poll(out);
        CHECK(out.empty()); // captured, not mapped
        REQUIRE(h.learnState() == ctl::LearnState::Captured);
        std::string why;
        REQUIRE(h.assignLearned(io::pad(5), why));
        CHECK(h.source("Odd Pads")->profileId() == "user-odd-pads");
        // Live at once: note 70 ch 3 is pad 5 now, and the profile's other bindings (Generic's) still work.
        CHECK(h.message(0, ev(0x92, 70, 90)));
        CHECK(h.message(0, ev(0x99, 36, 90)));
        h.poll(out);
        REQUIRE(out.size() == 2);
        CHECK((out[0].control == io::pad(5) && out[0].value == 90));
        // A second learn on an encoder CC with a relative first value: two's complement.
        REQUIRE(h.startLearn("Odd Pads"));
        CHECK(h.message(0, ev(0xB2, 21, 0x7F)));
        h.poll(out);
        REQUIRE(h.assignLearned(io::knob(2), why));
        h.inputClosed(0);
    }
    REQUIRE(access((dir + "/Controllers/user-odd-pads.json").c_str(), F_OK) == 0);
    ctl::ControllerHub restarted;
    restarted.log = [](const std::string&) {};
    restarted.setDataDir(dir);
    REQUIRE(restarted.profile("user-odd-pads") != nullptr);
    CHECK(restarted.profile("user-odd-pads")->portPattern == "Odd Pads");
    restarted.inputOpened(3, "Odd Pads");
    auto* src = restarted.source("Odd Pads");
    REQUIRE(src != nullptr);
    CHECK(src->profileId() == "user-odd-pads");
    CHECK(restarted.message(3, ev(0x92, 70, 64)));
    CHECK(restarted.message(3, ev(0xB2, 21, 0x7E)));
    std::vector<io::ControlEvent> out;
    restarted.poll(out);
    REQUIRE(out.size() == 2);
    CHECK((out[0].kind == Kind::Press && out[0].control == io::pad(5)));
    CHECK((out[1].kind == Kind::Turn && out[1].control == io::knob(2) && out[1].value == -2));
    // AUTO role matches the learned profile's port pattern even without the saved role.
    restarted.setRole("Odd Pads", ctl::PortRole::Auto);
    CHECK(restarted.source("Odd Pads") != nullptr);
    restarted.inputClosed(3);
    std::filesystem::remove_all(dir);
}

TEST_CASE("the keyboard remap is saved with the controller settings and loaded again", "[controllers]")
{
    const std::string dir = tempDir();
    auto& remap = ctl::keyRemap();
    remap.resetAll();
    {
        ctl::ControllerHub h;
        h.log = [](const std::string&) {};
        h.setDataDir(dir);
        remap.assign(io::pad(1), 'h', 0);
        std::vector<io::ControlEvent> out;
        h.poll(out); // saves (the remap's version changed)
    }
    remap.resetAll();
    CHECK(remap.keyFor(io::pad(1)).code == 'z');
    ctl::ControllerHub h;
    h.log = [](const std::string&) {};
    h.setDataDir(dir);
    CHECK(remap.keyFor(io::pad(1)).code == 'h');
    remap.resetAll();
    std::vector<io::ControlEvent> out;
    h.poll(out);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a broken user profile is skipped, never fatal", "[controllers]")
{
    const std::string dir = tempDir();
    std::filesystem::create_directories(dir + "/Controllers");
    std::ofstream(dir + "/Controllers/broken.json") << R"({"schema": 1, "id": "x", "in": [{"note": 1}]})";
    std::vector<std::string> lines;
    ctl::ControllerHub h;
    h.log = [&](const std::string& l) { lines.push_back(l); };
    h.setDataDir(dir);
    CHECK(h.profiles().size() == ctl::builtinProfiles().size());
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("broken.json skipped") != std::string::npos);
    std::filesystem::remove_all(dir);
}
