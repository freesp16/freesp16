// The reference unit's MIDI ports (platform/common/midi/UnitMidi.h, io/midi/MidiService): its cards by id or name,
// the /dev/din_midi real-time codes, the /proc/midi_through switch; DIN = slot 0 and USB = slot 1 whatever the ALSA
// card order, slot 1 kept for the USB gadget while the unit is in storage mode; THRU A on the DIN port switches the
// socket in hardware and the sender stops forwarding to it, while a THRU port fed by another input forwards in
// software.
#if defined(__linux__)
#include "platform/common/midi/UnitMidi.h"

#include "io/midi/MidiService.h"
#include "io/midi/MidiSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

namespace
{
namespace um = platform::unitmidi;

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A scripted port pair: the input hands out `pending` once, the output records writes and THRU switches.
struct FakePort
{
    std::string name;
    int slot = -1;
    bool present = true, hasThru = false;
    std::mutex m;
    std::vector<std::uint8_t> pending, written;
    std::vector<bool> thruCalls;
    bool thru = false;
};

class FakeInput final : public io::midi::MidiInput
{
public:
    explicit FakeInput(FakePort& p) : p_(p) {}
    int read(std::uint8_t* buf, int cap, int timeoutMs) override
    {
        {
            std::lock_guard<std::mutex> lock(p_.m);
            if (! p_.present)
                return -1;
            if (! p_.pending.empty())
            {
                const int n = std::min(cap, static_cast<int>(p_.pending.size()));
                std::copy(p_.pending.begin(), p_.pending.begin() + n, buf);
                p_.pending.erase(p_.pending.begin(), p_.pending.begin() + n);
                return n;
            }
        }
        sleepMs(timeoutMs < 5 ? timeoutMs : 5);
        return 0;
    }

private:
    FakePort& p_;
};

class FakeOutput final : public io::midi::MidiOutput
{
public:
    explicit FakeOutput(FakePort& p) : p_(p) {}
    bool write(const std::uint8_t* b, int n) override
    {
        std::lock_guard<std::mutex> lock(p_.m);
        p_.written.insert(p_.written.end(), b, b + n);
        return p_.present;
    }
    bool setHardwareThru(bool on) override
    {
        std::lock_guard<std::mutex> lock(p_.m);
        p_.thruCalls.push_back(on);
        if (! p_.hasThru)
            return ! on;
        p_.thru = on;
        return true;
    }

private:
    FakePort& p_;
};

class FakeProvider final : public io::midi::PortProvider
{
public:
    FakeProvider(std::string id, std::vector<FakePort*> ports, std::uint32_t reserved)
        : id_(std::move(id)), ports_(std::move(ports)), reserved_(reserved)
    {
    }
    std::string_view id() const override { return id_; }
    std::vector<io::midi::PortInfo> list() override
    {
        std::vector<io::midi::PortInfo> out;
        for (auto* p : ports_)
            if (p->present)
                out.push_back({"fake:" + p->name, p->name, true, true, p->slot});
        return out;
    }
    std::uint32_t reservedSlots() const override { return reserved_; }
    std::unique_ptr<io::midi::MidiInput> openInput(const io::midi::PortInfo& i, std::string&) override
    {
        return std::make_unique<FakeInput>(*find(i.name));
    }
    std::unique_ptr<io::midi::MidiOutput> openOutput(const io::midi::PortInfo& i, std::string&) override
    {
        return std::make_unique<FakeOutput>(*find(i.name));
    }

private:
    FakePort* find(const std::string& name)
    {
        for (auto* p : ports_)
            if (p->name == name)
                return p;
        return nullptr;
    }
    std::string id_;
    std::vector<FakePort*> ports_;
    std::uint32_t reserved_;
};

std::vector<std::uint8_t> writtenOf(FakePort& p)
{
    std::lock_guard<std::mutex> lock(p.m);
    return p.written;
}
} // namespace

TEST_CASE("midi.unit: the unit's cards, real-time codes and the OUT/THRU switch", "[midi]")
{
    CHECK(um::classify("snddinmidi", "") == um::UnitPort::Din);
    CHECK(um::classify("", "snd-din-midi") == um::UnitPort::Din);
    CHECK(um::classify("gmidi", "") == um::UnitPort::Usb);
    CHECK(um::classify("", "g_midi") == um::UnitPort::Usb);
    CHECK(um::classify("UM2", "UM-2") == um::UnitPort::None);
    CHECK(um::fixedSlot(um::UnitPort::Din) == 0);
    CHECK(um::fixedSlot(um::UnitPort::Usb) == 1);
    CHECK(um::fixedSlot(um::UnitPort::None) == -1);

    CHECK(um::kDinRealtimeIoctl == 0x40047000UL); // _IOW('p', 0, __u32)
    CHECK(um::realtimeCode(0xF8) == 0);
    CHECK(um::realtimeCode(0xFA) == 1);
    CHECK(um::realtimeCode(0xFB) == 2);
    CHECK(um::realtimeCode(0xFC) == 3);
    CHECK(um::realtimeCode(0xFE) == 4);
    CHECK(um::realtimeCode(0xFF) == 5);
    for (int b : {0x90, 0xF2, 0xF0, 0xF9, 0xFD, 0x00})
        CHECK(um::realtimeCode(static_cast<std::uint8_t>(b)) == -1);

    const auto path = std::filesystem::temp_directory_path() / ("midi-through-test-" + std::to_string(::getpid()));
    std::ofstream(path).put('x');
    const auto content = [&] {
        std::ifstream in(path);
        std::string s;
        std::getline(in, s);
        return s;
    };
    CHECK(um::setHardwareThru(true, path.c_str()));
    CHECK(content() == "1");
    CHECK(um::setHardwareThru(false, path.c_str()));
    CHECK(content() == "0");
    std::filesystem::remove(path);
    CHECK_FALSE(um::setHardwareThru(false, "/nonexistent-dir/midi_through"));
}

TEST_CASE("midi.unit: DIN = slot 0, USB = slot 1 in card order, kept in storage mode; THRU A in hardware", "[midi]")
{
    auto hub = std::make_unique<core::midi::MidiHub>();
    FakePort usb {.name = "MIDI USB", .slot = 1}, din {.name = "MIDI DIN", .slot = 0, .hasThru = true};
    FakePort keys {.name = "KEYS"}, pad {.name = "PAD"}, later {.name = "LATER", .present = false};
    std::vector<std::unique_ptr<io::midi::PortProvider>> providers;
    // A provider listed first with a port of its own, then the unit's cards in ALSA order (the gadget's card first).
    providers.push_back(std::make_unique<FakeProvider>("a-other", std::vector<FakePort*> {&pad, &later}, 0u));
    providers.push_back(std::make_unique<FakeProvider>("b-unit", std::vector<FakePort*> {&usb, &din, &keys}, 0b11u));
    io::midi::MidiService service(*hub, std::move(providers));
    std::string info;
    REQUIRE(service.start(info));
    INFO(info);
    CHECK(service.inputs()[0].name == "MIDI DIN");
    CHECK(service.outputs()[0].name == "MIDI DIN");
    CHECK(service.inputs()[1].name == "MIDI USB");
    CHECK(service.outputs()[1].name == "MIDI USB");
    CHECK(service.inputs()[2].name == "PAD");
    CHECK(service.inputs()[3].name == "KEYS");

    // Storage mode: the gadget vanishes; a port plugged meanwhile does not take its slot; the gadget comes back there.
    usb.present = false;
    later.present = true;
    service.refresh();
    CHECK_FALSE(service.outputs()[1].connected);
    CHECK(service.outputs()[1].name == "MIDI USB");
    CHECK(service.outputs()[4].name == "LATER");
    CHECK(service.outputs()[4].connected);
    usb.present = true;
    service.refresh();
    CHECK(service.outputs()[1].connected);
    CHECK(service.outputs()[1].name == "MIDI USB");

    // THRU A on the DIN output (its own input is A): the hardware switch, no forwarding to it.
    hub->setPortMode(0, 3);
    service.poll(0.0);
    CHECK(service.hardwareThru(0));
    {
        std::lock_guard<std::mutex> lock(din.m);
        REQUIRE_FALSE(din.thruCalls.empty());
        CHECK(din.thruCalls.back());
        CHECK(din.thru);
    }
    // THRU A on PAD's output too: another port, forwarded in software.
    hub->setPortMode(2, 3);
    service.poll(0.0);
    CHECK_FALSE(service.hardwareThru(2));
    {
        std::lock_guard<std::mutex> lock(din.m);
        din.pending = {0x90, 0x24, 0x64};
    }
    for (int i = 0; i < 200 && writtenOf(pad).size() < 3; ++i)
        sleepMs(5);
    CHECK(writtenOf(pad) == std::vector<std::uint8_t> {0x90, 0x24, 0x64});
    sleepMs(20);
    CHECK(writtenOf(din).empty()); // the hardware passed it on; the UART is not on the socket

    // MIDI PORTS › IN off for A: the socket goes back to the output; OUT mode: likewise.
    io::midi::setInputEnabled(0, false);
    service.poll(0.0);
    CHECK_FALSE(service.hardwareThru(0));
    io::midi::setInputEnabled(0, true);
    service.poll(0.0);
    CHECK(service.hardwareThru(0));
    hub->setPortMode(0, 0);
    service.poll(0.0);
    CHECK_FALSE(service.hardwareThru(0));
    {
        std::lock_guard<std::mutex> lock(din.m);
        CHECK_FALSE(din.thru);
    }
    // A port without a switch (USB): THRU B from its own input is asked of it, refused, and stays in software.
    hub->setPortMode(1, 4);
    service.poll(0.0);
    CHECK_FALSE(service.hardwareThru(1));
    {
        std::lock_guard<std::mutex> lock(usb.m);
        REQUIRE_FALSE(usb.thruCalls.empty());
        CHECK(usb.thruCalls.back());
        usb.pending = {0xB0, 0x07, 0x40};
    }
    for (int i = 0; i < 200 && writtenOf(usb).size() < 3; ++i)
        sleepMs(5);
    CHECK(writtenOf(usb) == std::vector<std::uint8_t> {0xB0, 0x07, 0x40});
    service.stop();
}
#endif
