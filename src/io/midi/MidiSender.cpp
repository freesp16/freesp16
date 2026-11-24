#include "io/midi/MidiSender.h"

#include "core/util/ThreadRole.h"
#include "io/midi/MidiWait.h"

#include <pthread.h>
#include <sched.h>

#include <cstdio>

namespace io::midi
{
void MidiSender::setOutput(int port, MidiOutput* output)
{
    if (port < 0 || port >= kPorts)
        return;
    std::lock_guard<core::PiMutex> lock(portsMutex_);
    outputs_[static_cast<std::size_t>(port)] = output;
    if (output != nullptr)
        clearFailed(port);
}

bool MidiSender::start(std::string& info)
{
    if (thread_.joinable())
        return true;
    quit_ = false;
    std::atomic<int> rt {-1};
    thread_ = std::thread([this, &rt] {
        rt = core::threads::apply(core::threads::Role::MidiOut, "midi-out").realtime ? 1 : 0; // best effort
        run();
    });
    while (rt.load() < 0) // the thread reports its priority at once (no atomic wait: GCC 11 / glibc 2.13 device)
        sched_yield();
    char buf[96];
    std::snprintf(buf, sizeof buf, "sender %s spin=%dus", rt.load() == 1 ? "SCHED_FIFO" : "normal-priority",
                  spinUs_.load());
    info = buf;
    return true;
}

void MidiSender::stop()
{
    if (! thread_.joinable())
        return;
    quit_ = true;
    thread_.join();
}

bool MidiSender::write(int port, const std::uint8_t* bytes, int size) noexcept
{
    std::lock_guard<core::PiMutex> lock(portsMutex_);
    MidiOutput* out = outputs_[static_cast<std::size_t>(port)];
    if (out == nullptr || failed(port))
        return false;
    if (! out->write(bytes, size))
    {
        failed_.fetch_or(1u << port);
        return false;
    }
    return true;
}

void MidiSender::record(double lateNs) noexcept
{
    const double us = lateNs < 0.0 ? 0.0 : lateNs / 1000.0;
    int bin = static_cast<int>(us / 10.0);
    bin = bin > kBins ? kBins : bin;
    hist_[static_cast<std::size_t>(bin)].fetch_add(1, std::memory_order_relaxed);
    samples_.fetch_add(1, std::memory_order_relaxed);
    if (lateNs > maxNs_.load(std::memory_order_relaxed))
        maxNs_.store(lateNs, std::memory_order_relaxed);
}

void MidiSender::forward() noexcept
{
    for (int in = 0; in < 2; ++in) // inputs A and B
    {
        core::midi::MidiEvent e;
        while (hub_.popForward(in, e))
            for (int p = 0; p < kPorts; ++p)
            {
                const int mode = hub_.portMode(p);
                const bool wants = (in == 0 && (mode == 1 || mode == 3)) || (in == 1 && (mode == 2 || mode == 4));
                const bool hardware = (hardwareThru_.load(std::memory_order_relaxed) >> p & 1u) != 0;
                if (wants && ! hardware && (hub_.outputMask() >> p & 1u) && write(p, e.bytes, e.size))
                    forwarded_.fetch_add(1, std::memory_order_relaxed);
            }
    }
    // Inputs C… are never forwarded: drain them so their queues do not fill.
    core::midi::MidiEvent e;
    for (int in = 2; in < kPorts; ++in)
        while (hub_.popForward(in, e))
        {
        }
}

bool MidiSender::sendNow(int port, const std::uint8_t* bytes, int size) noexcept
{
    if (port < 0 || port >= kPorts || size < 1 || size > 3)
        return false;
    core::midi::MidiOutEvent e;
    e.port = static_cast<std::uint8_t>(port);
    e.size = static_cast<std::uint8_t>(size);
    for (int i = 0; i < size; ++i)
        e.bytes[i] = bytes[i];
    return now_.push(e);
}

void MidiSender::run()
{
    constexpr std::int64_t kPollNs = 1000000; // 1 ms
    while (! quit_.load(std::memory_order_relaxed))
    {
        core::midi::MidiOutEvent e;
        while (hub_.popOut(e))
        {
            const int p = e.port & (kPorts - 1);
            if (count_[static_cast<std::size_t>(p)] == kRing)
            {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            ring_[static_cast<std::size_t>(p)][static_cast<std::size_t>((head_[static_cast<std::size_t>(p)] +
                                                                         count_[static_cast<std::size_t>(p)]) % kRing)] = e;
            ++count_[static_cast<std::size_t>(p)];
        }
        forward();
        for (core::midi::MidiOutEvent n; now_.pop(n);) // controller lamp feedback, written at once
            if ((hub_.outputMask() >> n.port & 1u) && write(n.port, n.bytes, n.size))
                sentNow_.fetch_add(1, std::memory_order_relaxed);
        const auto anchor = hub_.anchor();
        const int latency = hub_.outputLatencyMs();
        // The earliest deadline over the ports' queue heads.
        int best = -1;
        double bestNs = 0.0;
        for (int p = 0; p < kPorts; ++p)
            if (count_[static_cast<std::size_t>(p)] > 0)
            {
                const auto& h = ring_[static_cast<std::size_t>(p)][static_cast<std::size_t>(head_[static_cast<std::size_t>(p)])];
                const double d = deadlineNs(h, anchor, latency, hub_.portOffsetMs(p));
                if (best < 0 || d < bestNs)
                {
                    best = p;
                    bestNs = d;
                }
            }
        const std::int64_t now = core::midi::monotonicNs();
        if (best < 0 || ! anchor.valid)
        {
            waitUntilMonotonicNs(now + kPollNs);
            continue;
        }
        const auto deadline = static_cast<std::int64_t>(bestNs);
        const std::int64_t spin = static_cast<std::int64_t>(spinUs_.load(std::memory_order_relaxed)) * 1000;
        if (deadline - now > spin)
        {
            waitUntilMonotonicNs(deadline - spin < now + kPollNs ? deadline - spin : now + kPollNs);
            continue; // re-evaluate: new events or a new anchor may have arrived
        }
        while (core::midi::monotonicNs() < deadline) // the last stretch: busy wait to the deadline
        {
        }
        const auto& h = ring_[static_cast<std::size_t>(best)][static_cast<std::size_t>(head_[static_cast<std::size_t>(best)])];
        const bool ok = (hub_.outputMask() >> best & 1u) && write(best, h.bytes, h.size);
        const std::int64_t done = core::midi::monotonicNs();
        if (ok)
        {
            sent_.fetch_add(1, std::memory_order_relaxed);
            if (h.bytes[0] == 0xF8)
                clocks_.fetch_add(1, std::memory_order_relaxed);
            const double late = static_cast<double>(done) - bestNs;
            if (late > 1.0e6)
                late_.fetch_add(1, std::memory_order_relaxed);
            record(late);
        }
        else
            dropped_.fetch_add(1, std::memory_order_relaxed);
        head_[static_cast<std::size_t>(best)] = (head_[static_cast<std::size_t>(best)] + 1) % kRing;
        --count_[static_cast<std::size_t>(best)];
    }
}

SenderStats MidiSender::stats() const
{
    SenderStats s;
    s.sent = sent_.load();
    s.late = late_.load();
    s.forwarded = forwarded_.load();
    s.clocks = clocks_.load();
    s.dropped = dropped_.load();
    s.samples = samples_.load();
    s.maxUs = maxNs_.load() / 1000.0;
    std::uint64_t total = 0;
    std::array<std::uint32_t, kBins + 1> h {};
    for (int i = 0; i <= kBins; ++i)
    {
        h[static_cast<std::size_t>(i)] = hist_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        total += h[static_cast<std::size_t>(i)];
    }
    if (total == 0)
        return s;
    std::uint64_t acc = 0;
    bool p50 = false;
    for (int i = 0; i <= kBins; ++i)
    {
        acc += h[static_cast<std::size_t>(i)];
        if (! p50 && acc * 2 >= total)
        {
            s.p50Us = (i + 1) * 10.0;
            p50 = true;
        }
        if (acc * 100 >= total * 99)
        {
            s.p99Us = (i + 1) * 10.0;
            break;
        }
    }
    return s;
}

void MidiSender::resetStats()
{
    for (auto& b : hist_)
        b.store(0);
    sent_ = late_ = forwarded_ = clocks_ = dropped_ = samples_ = 0;
    maxNs_ = 0.0;
}
} // namespace io::midi
