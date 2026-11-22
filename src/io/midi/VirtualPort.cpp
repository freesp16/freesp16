#include "io/midi/VirtualPort.h"

#include "core/util/midi/MidiWire.h"

#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace io::midi
{
namespace
{
void sleepUntilNs(std::int64_t t) noexcept
{
    timespec ts {};
    ts.tv_sec = static_cast<time_t>(t / 1000000000LL);
    ts.tv_nsec = static_cast<long>(t % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0)
    {
    }
}

std::string trim(std::string_view s)
{
    while (! s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (! s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return std::string(s);
}
} // namespace

VirtualScript parseVirtualScript(std::string_view text)
{
    VirtualScript s;
    int lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string::npos)
            line = trim(std::string_view(line).substr(0, hash));
        if (line.empty())
        {
            if (end >= text.size())
                break;
            continue;
        }
        std::istringstream in(line);
        double at = 0;
        std::string word;
        if (! (in >> at >> word))
        {
            s.errors.push_back("line " + std::to_string(lineNo) + ": expected <ms> <step>");
            continue;
        }
        auto add = [&](std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, int n) {
            s.bytes.push_back({at, {b0, b1, b2}, n});
        };
        if (word == "clock")
        {
            VirtualScript::Clock c {at, 120.0, 1.0, 0.0};
            in >> c.bpm >> c.seconds;
            if (! (in >> c.jitterMs))
                c.jitterMs = 0.0;
            if (! (c.bpm > 0.0) || ! (c.seconds > 0.0))
                s.errors.push_back("line " + std::to_string(lineNo) + ": clock <bpm> <seconds> [jitter ms]");
            else
                s.clocks.push_back(c);
        }
        else if (word == "start")
            add(0xFA, 0, 0, 1);
        else if (word == "stop")
            add(0xFC, 0, 0, 1);
        else if (word == "continue")
            add(0xFB, 0, 0, 1);
        else if (word == "spp")
        {
            int v = 0;
            in >> v;
            v = std::clamp(v, 0, 16383);
            add(0xF2, static_cast<std::uint8_t>(v & 127), static_cast<std::uint8_t>(v >> 7), 3);
        }
        else
        {
            // Hex bytes: the word read first and the rest.
            std::vector<int> bytes;
            std::string h = word;
            do
            {
                char* stop = nullptr;
                const long b = std::strtol(h.c_str(), &stop, 16);
                if (stop == h.c_str() || *stop != '\0' || b < 0 || b > 255)
                {
                    bytes.clear();
                    break;
                }
                bytes.push_back(static_cast<int>(b));
            } while (in >> h);
            if (bytes.empty() || bytes.size() > 3)
                s.errors.push_back("line " + std::to_string(lineNo) + ": unknown step '" + word + "'");
            else
                add(static_cast<std::uint8_t>(bytes[0]), static_cast<std::uint8_t>(bytes.size() > 1 ? bytes[1] : 0),
                    static_cast<std::uint8_t>(bytes.size() > 2 ? bytes[2] : 0), static_cast<int>(bytes.size()));
        }
        if (end >= text.size())
            break;
    }
    std::stable_sort(s.bytes.begin(), s.bytes.end(), [](const auto& a, const auto& b) { return a.atMs < b.atMs; });
    return s;
}

struct VirtualProvider::Shared
{
    VirtualScript script;
    std::atomic<bool>* present = nullptr;
    mutable std::mutex mutex;
    std::vector<VirtualRecord> received;
};

namespace
{
class VirtualInput final : public MidiInput
{
public:
    explicit VirtualInput(std::shared_ptr<VirtualProvider::Shared> s) : s_(std::move(s))
    {
        t0_ = core::midi::monotonicNs();
        tick_.assign(s_->script.clocks.size(), 0);
        seed_ = 0x12345678u;
    }
    int read(std::uint8_t* buf, int cap, int timeoutMs) override
    {
        if (! s_->present->load())
            return -1;
        const std::int64_t now = core::midi::monotonicNs();
        const std::int64_t next = nextNs();
        const std::int64_t limit = now + static_cast<std::int64_t>(timeoutMs) * 1000000LL;
        if (next < 0 || next > limit)
        {
            sleepUntilNs(limit);
            return 0;
        }
        sleepUntilNs(next);
        int n = 0;
        const std::int64_t due = core::midi::monotonicNs();
        while (n + 3 <= cap)
        {
            int which = -1;
            const std::int64_t t = nextNs(&which);
            if (t < 0 || t > due)
                break;
            if (which < 0)
            {
                const auto& b = s_->script.bytes[byte_++];
                for (int i = 0; i < b.size; ++i)
                    buf[n++] = b.bytes[i];
            }
            else
            {
                buf[n++] = 0xF8;
                ++tick_[static_cast<std::size_t>(which)];
            }
        }
        return n;
    }

private:
    double tickJitterMs(std::size_t c, std::int64_t k)
    {
        // Deterministic per (clock, tick): a hash → uniform [−1, 1).
        std::uint32_t x = seed_ ^ static_cast<std::uint32_t>(c * 2654435761u) ^ static_cast<std::uint32_t>(k * 40503u);
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        const double u = static_cast<double>(x) / 4294967296.0 * 2.0 - 1.0;
        return u * s_->script.clocks[c].jitterMs;
    }
    // Monotonic ns of the next step (−1 = none); which = clock index or −1 for a byte step.
    std::int64_t nextNs(int* which = nullptr)
    {
        std::int64_t best = -1;
        int w = -1;
        if (byte_ < s_->script.bytes.size())
            best = t0_ + static_cast<std::int64_t>(s_->script.bytes[byte_].atMs * 1.0e6);
        for (std::size_t c = 0; c < s_->script.clocks.size(); ++c)
        {
            const auto& clk = s_->script.clocks[c];
            const std::int64_t k = tick_[c];
            const double periodMs = 60000.0 / (24.0 * clk.bpm);
            if (static_cast<double>(k) * periodMs >= clk.seconds * 1000.0)
                continue;
            const double atMs = clk.atMs + static_cast<double>(k) * periodMs + tickJitterMs(c, k);
            const std::int64_t t = t0_ + static_cast<std::int64_t>(atMs * 1.0e6);
            if (best < 0 || t < best)
            {
                best = t;
                w = static_cast<int>(c);
            }
        }
        if (which != nullptr)
            *which = w;
        return best;
    }

    std::shared_ptr<VirtualProvider::Shared> s_;
    std::int64_t t0_ = 0;
    std::size_t byte_ = 0;
    std::vector<std::int64_t> tick_;
    std::uint32_t seed_;
};

class VirtualOutput final : public MidiOutput
{
public:
    explicit VirtualOutput(std::shared_ptr<VirtualProvider::Shared> s) : s_(std::move(s)) {}
    bool write(const std::uint8_t* bytes, int size) override
    {
        if (! s_->present->load())
            return false;
        VirtualRecord r {core::midi::monotonicNs(), {0, 0, 0}, static_cast<std::uint8_t>(size)};
        for (int i = 0; i < size && i < 3; ++i)
            r.bytes[i] = bytes[i];
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->received.push_back(r);
        return true;
    }

private:
    std::shared_ptr<VirtualProvider::Shared> s_;
};
} // namespace

VirtualProvider::VirtualProvider(VirtualScript script, std::string outFile)
    : shared_(std::make_shared<Shared>()), outFile_(std::move(outFile))
{
    shared_->script = std::move(script);
    shared_->present = &present_;
    shared_->received.reserve(1u << 16);
}

VirtualProvider::~VirtualProvider()
{
    writeLog();
}

std::vector<PortInfo> VirtualProvider::list()
{
    if (! present_.load())
        return {};
    return {PortInfo {"virtual:0", "VIRTUAL", true, true}};
}

std::unique_ptr<MidiInput> VirtualProvider::openInput(const PortInfo&, std::string& error)
{
    if (! present_.load())
    {
        error = "virtual port detached";
        return nullptr;
    }
    return std::make_unique<VirtualInput>(shared_);
}

std::unique_ptr<MidiOutput> VirtualProvider::openOutput(const PortInfo&, std::string& error)
{
    if (! present_.load())
    {
        error = "virtual port detached";
        return nullptr;
    }
    return std::make_unique<VirtualOutput>(shared_);
}

std::vector<VirtualRecord> VirtualProvider::received() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->received;
}

bool VirtualProvider::writeLog() const
{
    if (outFile_.empty())
        return false;
    const auto records = received();
    FILE* f = std::fopen(outFile_.c_str(), "w");
    if (f == nullptr)
        return false;
    for (const auto& r : records)
    {
        std::fprintf(f, "%lld", static_cast<long long>(r.atNs));
        for (int i = 0; i < r.size; ++i)
            std::fprintf(f, " %02X", r.bytes[i]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}
} // namespace io::midi
