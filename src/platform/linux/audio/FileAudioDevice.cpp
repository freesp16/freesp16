#include "platform/linux/audio/FileAudioDevice.h"

#include "core/util/Realtime.h"
#include "core/util/ThreadRole.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace platform::linux_desktop
{
namespace
{
inline std::int16_t toS16(float x) noexcept // as AlsaOutput: round to nearest with saturation, no libm
{
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    const float s = x * 32767.0f;
    return static_cast<std::int16_t>(static_cast<int>(s + (s >= 0.0f ? 0.5f : -0.5f)));
}

void put16(unsigned char* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>(v >> 8);
}
void put32(unsigned char* p, std::uint32_t v) noexcept
{
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
}
} // namespace

bool FileAudioDevice::writeHeader(std::uint32_t dataBytes, bool rewind)
{
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1); // PCM
    put16(h + 22, static_cast<std::uint16_t>(channels_));
    put32(h + 24, config_.sampleRate);
    put32(h + 28, config_.sampleRate * channels_ * 2);
    put16(h + 32, static_cast<std::uint16_t>(channels_ * 2));
    put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
    return (! rewind || std::fseek(file_, 0, SEEK_SET) == 0) && std::fwrite(h, 1, sizeof h, file_) == sizeof h;
}

namespace
{
std::uint32_t get32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t get16(const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}
} // namespace

// The input stand-in: a whole 16-bit PCM WAV (mono → both sides, MIX-052 "L only"), planar.
bool FileAudioDevice::loadInput(std::string& info)
{
    inL_.clear();
    inR_.clear();
    if (options_.inputPath.empty())
        return true;
    std::FILE* f = std::fopen(options_.inputPath.c_str(), "rb");
    if (f == nullptr)
    {
        info = "file: cannot open input " + options_.inputPath + ": " + std::strerror(errno);
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        info = "file: input " + options_.inputPath + " is not a WAV file";
        return false;
    }
    unsigned channels = 0, bits = 0, format = 0, rate = 0;
    const unsigned char* data = nullptr;
    std::size_t dataBytes = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();)
    {
        const std::size_t size = get32(bytes.data() + at + 4);
        const unsigned char* body = bytes.data() + at + 8;
        const std::size_t avail = std::min(size, bytes.size() - at - 8);
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && avail >= 16)
        {
            format = get16(body);
            channels = get16(body + 2);
            rate = get32(body + 4);
            bits = get16(body + 14);
        }
        else if (std::memcmp(bytes.data() + at, "data", 4) == 0)
        {
            data = body;
            dataBytes = avail;
        }
        at += 8 + size + (size & 1);
    }
    if (format != 1 || bits != 16 || channels < 1 || channels > 2 || data == nullptr)
    {
        info = "file: input " + options_.inputPath + ": only 16-bit PCM mono/stereo WAV is supported";
        return false;
    }
    const std::size_t frames = dataBytes / (2 * channels);
    inL_.resize(frames);
    inR_.resize(frames);
    for (std::size_t i = 0; i < frames; ++i)
    {
        const auto* p = data + 2 * channels * i;
        inL_[i] = static_cast<float>(static_cast<std::int16_t>(get16(p))) / 32768.0f;
        inR_[i] = channels == 2 ? static_cast<float>(static_cast<std::int16_t>(get16(p + 2))) / 32768.0f : inL_[i];
    }
    info = "input " + options_.inputPath + " (" + std::to_string(frames) + " frames, " + std::to_string(channels)
         + " ch, " + std::to_string(rate) + " Hz" + (rate != config_.sampleRate ? ", RATE NOT CONVERTED" : "") + ", looped)";
    return true;
}

bool FileAudioDevice::start(const AudioConfig& config, engine::AudioProcessor& processor, std::string& info)
{
    stop();
    config_ = config;
    processor_ = &processor;
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr)
    {
        info = "file: cannot create " + path_ + ": " + std::strerror(errno);
        return false;
    }
    if (! writeHeader(0, false)) // the file was just opened at 0: no seek, so a FIFO works too (tools/soak)
    {
        info = "file: cannot write " + path_;
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    std::string inputInfo;
    if (! loadInput(inputInfo))
    {
        info = inputInfo;
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    const auto frames = static_cast<std::size_t>(config.periodFrames);
    planar_.assign(channels_ * frames, 0.0f);
    out_.fill(nullptr);
    for (unsigned c = 0; c < channels_; ++c) // AudioPort order: out_[c] = WAV channel c
        out_[c] = planar_.data() + c * frames;
    inPeriod_.assign(2 * frames, 0.0f);
    in_ = {inPeriod_.data(), inPeriod_.data() + frames};
    inPos_ = 0;
    const double perSecond = static_cast<double>(config.sampleRate) / std::max(1u, config.periodFrames);
    const auto periodsInRing = static_cast<std::size_t>(std::max(4.0, ringSeconds_ * perSecond));
    ringFrames_ = periodsInRing * frames;
    ring_.assign(ringFrames_ * channels_, 0);
    processor.prepare(static_cast<double>(config.sampleRate), static_cast<int>(config.periodFrames));
    head_ = 0;
    tail_ = 0;
    periods_ = 0;
    xruns_ = 0;
    late_ = 0;
    written_ = 0;
    writeFailed_ = false;
    quitDevice_ = false;
    quitWriter_ = false;
    writer_ = std::thread([this] { runWriter(); });
    device_ = std::thread([this] { runDevice(); });
    info = "file: " + path_ + " S16_LE x" + std::to_string(channels_) + " " + std::to_string(config.sampleRate)
         + " Hz, period " + std::to_string(config.periodFrames) + " frames, paced in real time, ring "
         + std::to_string(ringFrames_) + " frames, MAIN = ch 0/1"
         + (options_.allPorts ? ", OUT3-4 = 2/3, OUT5-6 = 4/5, OUT7-8 = 6/7, PHONES = 8/9, FILTER SEND = 10/11" : "")
         + (inputInfo.empty() ? "" : ", " + inputInfo);
    return true;
}

void FileAudioDevice::stop()
{
    quitDevice_ = true;
    if (device_.joinable())
        device_.join();
    quitWriter_ = true;
    if (writer_.joinable())
        writer_.join(); // drains the ring
    if (file_ != nullptr)
    {
        const auto bytes = // RIFF sizes are 32-bit: a > 4 GB recording keeps a capped (but readable) header
            static_cast<std::uint32_t>(std::min<std::uint64_t>(written_.load() * channels_ * 2, 0xFFFFFFD3u));
        writeHeader(bytes); // fails on a FIFO (not seekable): the reader has the stream, no size patch needed
        std::fclose(file_);
        file_ = nullptr;
    }
}

AudioStats FileAudioDevice::stats() const noexcept
{
    return {periods_.load(std::memory_order_relaxed), xruns_.load(std::memory_order_relaxed), config_.sampleRate,
            config_.periodFrames, realtime_.load(std::memory_order_relaxed)};
}

void FileAudioDevice::runDevice()
{
    realtime_ = core::threads::apply(core::threads::Role::Audio, "audio").realtime; // best effort
    core::rt::enableFlushToZero();
    core::rt::ScopedRealtimeThread realtime;
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(config_.periodFrames) / config_.sampleRate));
    const std::size_t frames = config_.periodFrames;
    const bool input = ! inL_.empty();
    const engine::AudioIO io {out_.data(), static_cast<int>(AudioPort::Count), input ? in_.data() : nullptr,
                              input ? 2 : 0, static_cast<int>(frames), 0};
    const unsigned ch = channels_;
    auto next = clock::now();
    while (! quitDevice_.load(std::memory_order_relaxed))
    {
        engine::AudioIO now = io;
        now.hostTimeNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
        if (input) // one period of the looped input file (pre-loaded: no I/O here)
            for (std::size_t i = 0; i < frames; ++i)
            {
                inPeriod_[i] = inL_[inPos_];
                inPeriod_[frames + i] = inR_[inPos_];
                inPos_ = inPos_ + 1 < inL_.size() ? inPos_ + 1 : 0;
            }
        processor_->process(now);
        const auto head = head_.load(std::memory_order_relaxed);
        if (head + frames - tail_.load(std::memory_order_acquire) > ringFrames_)
            xruns_.fetch_add(1, std::memory_order_relaxed); // ring full: this period is lost
        else
        {
            // The ring holds whole periods and head advances by one period, so a period never wraps.
            std::int16_t* frame0 = ring_.data() + static_cast<std::size_t>(head % ringFrames_) * ch;
            for (unsigned c = 0; c < ch; ++c)
            {
                const float* x = out_[c];
                std::int16_t* dst = frame0 + c;
                for (std::size_t i = 0; i < frames; ++i, dst += ch)
                    *dst = toS16(x[i]);
            }
            head_.store(head + frames, std::memory_order_release);
        }
        periods_.fetch_add(1, std::memory_order_relaxed);
        next += period;
        const auto t = clock::now();
        if (t > next + period)
        {
            late_.fetch_add(1, std::memory_order_relaxed);
            next = t;
        }
        std::this_thread::sleep_until(next);
    }
}

void FileAudioDevice::runWriter()
{
    (void) core::threads::apply(core::threads::Role::Recorder, "file-writer"); // drains the ring to the file
    for (;;)
    {
        const bool last = quitWriter_.load(std::memory_order_acquire); // the device thread has stopped: drain all
        const auto head = head_.load(std::memory_order_acquire);
        auto tail = tail_.load(std::memory_order_relaxed);
        while (tail < head)
        {
            const std::size_t at = static_cast<std::size_t>(tail % ringFrames_);
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(head - tail), ringFrames_ - at);
            const std::int16_t* frames = ring_.data() + at * channels_;
            if (! writeFailed_ && std::fwrite(frames, sizeof(std::int16_t) * channels_, n, file_) != n)
                writeFailed_ = true;
            if (! writeFailed_)
                written_.fetch_add(n, std::memory_order_relaxed);
            tail += n;
            tail_.store(tail, std::memory_order_release);
        }
        if (last)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
} // namespace platform::linux_desktop
