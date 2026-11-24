#include "io/midi/MidiService.h"
#include "io/midi/MidiSettings.h"

#include "io/midi/InputRouter.h"

#include "core/util/ThreadRole.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

namespace io::midi
{
namespace
{
std::vector<PortProviderEntry>& registry()
{
    static std::vector<PortProviderEntry> entries;
    return entries;
}
} // namespace

bool registerPortProvider(std::string_view id, PortProviderFactory factory)
{
    for (const auto& e : registry())
        if (e.id == id)
            return false;
    registry().push_back({id, factory});
    return true;
}

std::vector<PortProviderEntry> portProviders()
{
    auto entries = registry();
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return entries;
}

MidiService::MidiService(core::midi::MidiHub& hub, std::vector<std::unique_ptr<PortProvider>> providers)
    : hub_(hub), providers_(std::move(providers)), sender_(hub)
{
}

MidiService::~MidiService() { stop(); }

bool MidiService::start(std::string& info)
{
    if (started_)
        return true;
    started_ = true;
    std::string senderInfo;
    sender_.start(senderInfo);
    refresh();
    info = describe() + " " + senderInfo;
    return true;
}

void MidiService::stop()
{
    if (! started_)
        return;
    started_ = false;
    sender_.stop();
    for (int i = 0; i < kPorts; ++i)
    {
        closeInput(i);
        closeOutput(i);
    }
}

int MidiService::slotFor(std::array<Slot, kPorts>& slots, const std::string& provider, const std::string& name)
{
    for (int i = 0; i < kPorts; ++i)
        if (slots[static_cast<std::size_t>(i)].name == name && slots[static_cast<std::size_t>(i)].provider == provider)
            return i;
    for (int i = 0; i < kPorts; ++i)
        if (slots[static_cast<std::size_t>(i)].name.empty())
        {
            slots[static_cast<std::size_t>(i)].name = name;
            slots[static_cast<std::size_t>(i)].provider = provider;
            return i;
        }
    return -1; // all 8 slots are taken by other names
}

void MidiService::inputLoop(int slot, MidiInput* port, Input* state)
{
    char name[16];
    std::snprintf(name, sizeof name, "midi-in-%d", slot);
    (void) core::threads::apply(core::threads::Role::MidiIn, name); // best effort (refused: normal priority)
    MidiParser parser;
    std::uint8_t buf[256];
    while (! state->quit.load(std::memory_order_relaxed))
    {
        const int n = port->read(buf, static_cast<int>(sizeof buf), 100);
        if (n < 0)
        {
            state->gone = true; // unplugged: the next refresh closes the slot
            return;
        }
        if (n == 0)
            continue;
        const std::int64_t stamp = core::midi::monotonicNs(); // arrival (threading.md: parse + timestamp only)
        InputRouter* router = inputRouter(); // controller profiles consume their messages (UI-136)
        if (router != nullptr)
            router->rawBytes(slot, buf, n, stamp);
        core::midi::MidiEvent e;
        for (int i = 0; i < n; ++i)
            if (parser.feed(buf[i], e) && inputEnabled(slot)) // MIDI PORTS › IN off = dropped
            {
                e.port = static_cast<std::uint8_t>(slot);
                e.hostTimeNs = stamp;
                if (router != nullptr && router->message(slot, e))
                {
                    messagesIn_.fetch_add(1, std::memory_order_relaxed);
                    continue; // a controller's message, not the MIDI input's
                }
                hub_.pushIn(slot, e);
                hub_.pushForward(slot, e);
                messagesIn_.fetch_add(1, std::memory_order_relaxed);
            }
    }
}

void MidiService::closeInput(int slot)
{
    auto& in = inputs_[static_cast<std::size_t>(slot)];
    if (in.thread.joinable())
    {
        in.quit = true;
        in.thread.join();
    }
    in.port.reset();
    in.quit = false;
    in.gone = false;
    if (inSlots_[static_cast<std::size_t>(slot)].connected)
    {
        hub_.setInputConnected(slot, false);
        if (auto* router = inputRouter())
            router->inputClosed(slot);
    }
    inSlots_[static_cast<std::size_t>(slot)].connected = false;
}

void MidiService::closeOutput(int slot)
{
    sender_.setOutput(slot, nullptr);
    outputs_[static_cast<std::size_t>(slot)].reset();
    if (outSlots_[static_cast<std::size_t>(slot)].connected)
    {
        hub_.setOutputConnected(slot, false);
        if (auto* router = inputRouter())
            router->outputClosed(slot);
    }
    outSlots_[static_cast<std::size_t>(slot)].connected = false;
}

void MidiService::refresh()
{
    // Vanished or failed ports first.
    for (int i = 0; i < kPorts; ++i)
    {
        if (inSlots_[static_cast<std::size_t>(i)].connected && inputs_[static_cast<std::size_t>(i)].gone.load())
            closeInput(i);
        if (outSlots_[static_cast<std::size_t>(i)].connected && sender_.failed(i))
            closeOutput(i);
    }
    std::array<bool, kPorts> seenIn {}, seenOut {};
    for (auto& provider : providers_)
    {
        const std::string pid(provider->id());
        for (const auto& info : provider->list())
        {
            if (info.input)
            {
                const int s = slotFor(inSlots_, pid, info.name);
                if (s >= 0)
                {
                    seenIn[static_cast<std::size_t>(s)] = true;
                    if (! inSlots_[static_cast<std::size_t>(s)].connected)
                    {
                        std::string error;
                        if (auto port = provider->openInput(info, error))
                        {
                            auto& in = inputs_[static_cast<std::size_t>(s)];
                            in.port = std::move(port);
                            in.quit = false;
                            in.gone = false;
                            in.thread = std::thread(&MidiService::inputLoop, this, s, in.port.get(), &in);
                            inSlots_[static_cast<std::size_t>(s)].connected = true;
                            hub_.setInputConnected(s, true);
                            ++reconnects_;
                            if (auto* router = inputRouter())
                                router->inputOpened(s, info.name);
                        }
                    }
                }
            }
            if (info.output)
            {
                const int s = slotFor(outSlots_, pid, info.name);
                if (s >= 0)
                {
                    seenOut[static_cast<std::size_t>(s)] = true;
                    if (! outSlots_[static_cast<std::size_t>(s)].connected)
                    {
                        std::string error;
                        if (auto port = provider->openOutput(info, error))
                        {
                            outputs_[static_cast<std::size_t>(s)] = std::move(port);
                            sender_.setOutput(s, outputs_[static_cast<std::size_t>(s)].get());
                            outSlots_[static_cast<std::size_t>(s)].connected = true;
                            hub_.setOutputConnected(s, true); // a new generation: the guard ends old notes
                            ++reconnects_;
                            if (auto* router = inputRouter())
                                router->outputOpened(s, info.name, sender_);
                        }
                    }
                }
            }
        }
    }
    // Ports no provider lists any more (unplugged without an error yet).
    for (int i = 0; i < kPorts; ++i)
    {
        if (inSlots_[static_cast<std::size_t>(i)].connected && ! seenIn[static_cast<std::size_t>(i)])
            closeInput(i);
        if (outSlots_[static_cast<std::size_t>(i)].connected && ! seenOut[static_cast<std::size_t>(i)])
            closeOutput(i);
    }
    // A router installed after the ports opened learns about them now.
    if (auto* router = inputRouter(); router != notified_)
    {
        notified_ = router;
        if (router != nullptr)
            for (int i = 0; i < kPorts; ++i)
            {
                if (inSlots_[static_cast<std::size_t>(i)].connected)
                    router->inputOpened(i, inSlots_[static_cast<std::size_t>(i)].name);
                if (outSlots_[static_cast<std::size_t>(i)].connected)
                    router->outputOpened(i, outSlots_[static_cast<std::size_t>(i)].name, sender_);
            }
    }
}

void MidiService::poll(double nowMs)
{
    if (! started_ || nowMs - lastRefreshMs_ < kRefreshMs)
        return;
    lastRefreshMs_ = nowMs;
    refresh();
}

std::string MidiService::describe() const
{
    std::string s = "in:";
    for (int i = 0; i < kPorts; ++i)
        if (! inSlots_[static_cast<std::size_t>(i)].name.empty())
            s += " " + std::to_string(i + 1) + "=" + inSlots_[static_cast<std::size_t>(i)].name +
                 (inSlots_[static_cast<std::size_t>(i)].connected ? "" : "(off)");
    s += " out:";
    for (int i = 0; i < kPorts; ++i)
        if (! outSlots_[static_cast<std::size_t>(i)].name.empty())
            s += " " + std::to_string(i + 1) + "=" + outSlots_[static_cast<std::size_t>(i)].name +
                 (outSlots_[static_cast<std::size_t>(i)].connected ? "" : "(off)");
    return s;
}
} // namespace io::midi
