#include "io/midi/MidiSender.h"

#include <sched.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <mutex>

namespace io::midi
{
namespace
{
constexpr std::int64_t kPollNs = 1000000; // 1 ms: the longest sleep, so new messages are seen in time
} // namespace

// ---- PiMutex ----

PiMutex::PiMutex() noexcept
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT); // best effort: a plain mutex without it
    pthread_mutex_init(&m_, &a);
    pthread_mutexattr_destroy(&a);
}

PiMutex::~PiMutex()
{
    pthread_mutex_destroy(&m_);
}

// ---- LatenessHistogram ----

void LatenessHistogram::record(double lateNs) noexcept
{
    const double us = lateNs < 0.0 ? 0.0 : lateNs / 1000.0;
    int bin = static_cast<int>(us / 10.0);
    bin = bin > kBins ? kBins : bin;
    bins_[static_cast<std::size_t>(bin)].fetch_add(1, std::memory_order_relaxed);
    samples_.fetch_add(1, std::memory_order_relaxed);
    if (lateNs > maxNs_.load(std::memory_order_relaxed))
        maxNs_.store(lateNs, std::memory_order_relaxed);
}

double LatenessHistogram::percentileUs(double q) const noexcept
{
    std::uint64_t total = 0;
    for (const auto& b : bins_)
        total += b.load(std::memory_order_relaxed);
    if (total == 0)
        return 0.0;
    const auto want = static_cast<double>(total) * q;
    std::uint64_t acc = 0;
    for (int i = 0; i <= kBins; ++i)
    {
        acc += bins_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        if (static_cast<double>(acc) >= want)
            return (i + 1) * 10.0;
    }
    return (kBins + 1) * 10.0;
}

void LatenessHistogram::reset() noexcept
{
    for (auto& b : bins_)
        b.store(0, std::memory_order_relaxed);
    samples_.store(0, std::memory_order_relaxed);
    maxNs_.store(0.0, std::memory_order_relaxed);
}

// ---- DeadlineWaiter ----

void DeadlineWaiter::learn(std::int64_t latencyNs, std::int64_t sinceNs) noexcept
{
    const double decay = sinceNs > 0 ? std::exp(-static_cast<double>(sinceNs) / (kDecaySeconds * 1.0e9)) : 1.0;
    const double late = latencyNs > 0 ? static_cast<double>(latencyNs) : 0.0;
    peakNs_ = peakNs_ * decay > late ? peakNs_ * decay : late;
    const std::int64_t lo = floor_.load(std::memory_order_relaxed);
    const std::int64_t hi = lo > kMaxMarginNs ? lo : kMaxMarginNs;
    const auto m = static_cast<std::int64_t>(peakNs_ * 1.5) + 50000;
    margin_.store(m < lo ? lo : (m > hi ? hi : m), std::memory_order_relaxed);
}

void DeadlineWaiter::setFloorNs(std::int64_t ns) noexcept
{
    floor_.store(ns < 0 ? 0 : ns, std::memory_order_relaxed);
    if (margin_.load(std::memory_order_relaxed) < ns)
        margin_.store(ns, std::memory_order_relaxed);
}

void DeadlineWaiter::sleepUntil(std::int64_t t, std::int64_t now) noexcept
{
    timespec ts {};
    ts.tv_sec = static_cast<time_t>(t / 1000000000LL);
    ts.tv_nsec = static_cast<long>(t % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0)
    {
    }
    const std::int64_t woke = core::midi::monotonicNs();
    if (t > now) // a real sleep: how late the kernel woke this thread
        learn(woke - t, lastWake_ > 0 ? woke - lastWake_ : 0);
    lastWake_ = woke;
}

void DeadlineWaiter::spinUntil(std::int64_t deadline) noexcept
{
    while (core::midi::monotonicNs() < deadline)
    {
    }
}

// ---- MidiSender ----

bool MidiSender::Lanes::push(const core::midi::MidiOutEvent& e) noexcept
{
    const auto p = static_cast<std::size_t>(e.port & (kPorts - 1));
    if (count[p] == kRing)
        return false;
    ring[p][static_cast<std::size_t>((head[p] + count[p]) % kRing)] = e;
    ++count[p];
    return true;
}

void MidiSender::Lanes::pop(int port) noexcept
{
    const auto p = static_cast<std::size_t>(port);
    head[p] = (head[p] + 1) % kRing;
    --count[p];
    retryAt[p] = 0;
}

void MidiSender::setOutput(int port, MidiOutput* output)
{
    if (port < 0 || port >= kPorts)
        return;
    std::lock_guard<PiMutex> lock(portLocks_[static_cast<std::size_t>(port)]);
    outputs_[static_cast<std::size_t>(port)] = output;
    if (output != nullptr)
        clearFailed(port);
}

bool MidiSender::startThread(std::thread& t, int priority, void (MidiSender::*body)())
{
    std::atomic<int> rt {-1};
    t = std::thread(
        [this, priority, body, &rt]
        {
            sched_param p{};
            p.sched_priority = priority;
            rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0 ? 1 : 0; // best effort
            (this->*body)();
        });
    while (rt.load() < 0) // the thread reports its priority at once (no atomic wait: GCC 11 / glibc 2.13 device)
        sched_yield();
    return rt.load() == 1;
}

bool MidiSender::start(std::string& info)
{
    if (sender_.joinable())
    {
        info = info_;
        return true;
    }
    quit_ = false;
    const bool clockRt = startThread(clock_, kClockPriority, &MidiSender::runClock);
    const bool senderRt = startThread(sender_, kSenderPriority, &MidiSender::runSender);
    char buf[128];
    std::snprintf(buf, sizeof buf, "clock %s sender %s wake-margin=%lldus(adaptive)",
                  clockRt ? "SCHED_FIFO 80" : "normal-priority", senderRt ? "SCHED_FIFO 72" : "normal-priority",
                  static_cast<long long>(clockWait_.margin() / 1000));
    info_ = info = buf;
    return true;
}

void MidiSender::stop()
{
    if (! sender_.joinable())
        return;
    quit_ = true;
    sender_.join();
    clock_.join();
}

WriteResult MidiSender::write(int port, const std::uint8_t* bytes, int size) noexcept
{
    std::lock_guard<PiMutex> lock(portLocks_[static_cast<std::size_t>(port)]);
    MidiOutput* out = outputs_[static_cast<std::size_t>(port)];
    if (out == nullptr || failed(port))
        return WriteResult::Gone;
    const WriteResult r = out->write(bytes, size);
    if (r == WriteResult::Gone)
        failed_.fetch_or(1u << port);
    return r;
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
                if (wants && ! hardware && (hub_.outputMask() >> p & 1u) &&
                    write(p, e.bytes, e.size) == WriteResult::Written)
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

void MidiSender::serve(Lane lane, Lanes& lanes, DeadlineWaiter& waiter)
{
    const auto anchor = hub_.anchor();
    const int latency = hub_.outputLatencyMs();
    // The earliest head over the ports: its deadline, or a busy port's retry time when that is later.
    int best = -1;
    double bestDue = 0.0, bestDeadline = 0.0, bestRaw = 0.0;
    for (int p = 0; p < kPorts; ++p)
        if (lanes.count[static_cast<std::size_t>(p)] > 0)
        {
            const auto& h = lanes.front(p);
            const double raw = deadlineNs(h, anchor, latency, hub_.portOffsetMs(p));
            const double d =
                lane == Lane::Clock && h.bytes[0] == 0xF8
                    ? lanes.ticks[static_cast<std::size_t>(p)].deadline(raw, h.exactTime(), anchor.nsPerFrame)
                    : raw;
            const auto retry = static_cast<double>(lanes.retryAt[static_cast<std::size_t>(p)]);
            const double due = retry > d ? retry : d;
            if (best < 0 || due < bestDue)
            {
                best = p;
                bestDue = due;
                bestDeadline = d;
                bestRaw = raw;
            }
        }
    const std::int64_t now = core::midi::monotonicNs();
    if (best < 0 || ! anchor.valid)
    {
        waiter.sleepUntil(now + kPollNs, now);
        return;
    }
    const auto due = static_cast<std::int64_t>(bestDue);
    if (due - now > waiter.margin())
    {
        const std::int64_t wake = waiter.wakeFor(due);
        waiter.sleepUntil(wake < now + kPollNs ? wake : now + kPollNs, now);
        return; // re-evaluate: new messages or a new anchor may have arrived
    }
    DeadlineWaiter::spinUntil(due); // the last stretch: busy wait to the deadline
    const auto& h = lanes.front(best);
    const WriteResult r = (hub_.outputMask() >> best & 1u) ? write(best, h.bytes, h.size) : WriteResult::Gone;
    const std::int64_t done = core::midi::monotonicNs();
    if (r == WriteResult::Busy)
    {
        busy_.fetch_add(1, std::memory_order_relaxed);
        if (lane == Lane::Clock && static_cast<double>(done) - bestDeadline > static_cast<double>(kStaleNs))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed); // stale: a late burst of ticks would be worse
            popClock(lanes, best, bestRaw, anchor.nsPerFrame);
        }
        else
            lanes.retryAt[static_cast<std::size_t>(best)] = done + kRetryNs;
        return;
    }
    if (r == WriteResult::Written)
    {
        sent_.fetch_add(1, std::memory_order_relaxed);
        if (h.bytes[0] == 0xF8)
            clocks_.fetch_add(1, std::memory_order_relaxed);
        const double late = static_cast<double>(done) - bestDeadline;
        if (late > 1.0e6)
            late_.fetch_add(1, std::memory_order_relaxed);
        (lane == Lane::Clock ? clockLate_ : noteLate_).record(late);
    }
    else
        dropped_.fetch_add(1, std::memory_order_relaxed);
    if (lane == Lane::Clock)
        popClock(lanes, best, bestRaw, anchor.nsPerFrame);
    else
        lanes.pop(best);
}

void MidiSender::popClock(Lanes& lanes, int port, double raw, double nsPerFrame) noexcept
{
    const auto& h = lanes.front(port);
    auto& ticks = lanes.ticks[static_cast<std::size_t>(port)];
    if (h.bytes[0] == 0xF8)
        ticks.commit(raw, h.exactTime(), nsPerFrame); // the next tick is due from here
    else
        ticks.reset(); // Start / Continue / Stop / Song Position: the next tick starts afresh
    lanes.pop(port);
}

void MidiSender::runClock()
{
    while (! quit_.load(std::memory_order_relaxed))
    {
        for (core::midi::MidiOutEvent e; hub_.popSync(e);)
            if (! clockLanes_.push(e))
                dropped_.fetch_add(1, std::memory_order_relaxed);
        serve(Lane::Clock, clockLanes_, clockWait_);
    }
}

void MidiSender::runSender()
{
    while (! quit_.load(std::memory_order_relaxed))
    {
        for (core::midi::MidiOutEvent e; hub_.popOut(e);)
            if (! noteLanes_.push(e))
                dropped_.fetch_add(1, std::memory_order_relaxed);
        forward();
        for (core::midi::MidiOutEvent n; now_.pop(n);) // controller lamp feedback, written at once
            if ((hub_.outputMask() >> n.port & 1u) && write(n.port, n.bytes, n.size) == WriteResult::Written)
                sentNow_.fetch_add(1, std::memory_order_relaxed);
        serve(Lane::Notes, noteLanes_, noteWait_);
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
    s.busy = busy_.load();
    s.samples = clockLate_.samples() + noteLate_.samples();
    s.p50Us = clockLate_.percentileUs(0.5);
    s.p99Us = clockLate_.percentileUs(0.99);
    s.maxUs = clockLate_.maxUs();
    s.notesP99Us = noteLate_.percentileUs(0.99);
    s.marginUs = static_cast<double>(clockWait_.margin()) / 1000.0;
    return s;
}

void MidiSender::resetStats()
{
    clockLate_.reset();
    noteLate_.reset();
    sent_ = late_ = forwarded_ = clocks_ = dropped_ = busy_ = 0;
}
} // namespace io::midi
