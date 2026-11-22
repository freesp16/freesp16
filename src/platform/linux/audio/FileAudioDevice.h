// The `file` audio device: renders in real time into a WAV file instead of a sound card — what the automated
// checks (scripts/host-smoke.sh, tools/analyze-wav.py) listen to. Selected by audio_device=file:<path.wav>.
//
// Threads: the device thread is paced by a period clock (sleep_until), calls AudioProcessor::process once per period
// under the real-time contract (ScopedRealtimeThread, FTZ; SCHED_FIFO best effort) and converts the output into a
// pre-allocated lock-free single-producer/single-consumer ring of interleaved S16 frames — no allocation, lock or I/O
// on it. A non-real-time writer thread streams the ring to the file; stop() drains it and fixes the RIFF/data sizes.
// A full ring (the writer or the disk fell behind) drops that period and counts it as an xrun.
//
// Format: 16-bit PCM (tools/analyze-wav.py reads 16-bit), 2 channels = MAIN L/R (WAV channels 0/1); the other
// ports are absent in this device's port map, so the engine does not render them and nothing else is written.
// With FileAudioOptions::allPorts (setting audio_file_ports=all) the WAV has 12 channels, every
// logical port in AudioPort order — MAIN 0/1 (unchanged), OUT3-4 2/3, OUT5-6 4/5, OUT7-8 6/7, PHONES 8/9, FILTER SEND
// 10/11 — so the routing checks listen to each output (tools/analyze-wav.py --channels 2,3 …).
// Input stand-in (no ALSA capture on the desktop): FileAudioOptions::inputPath (setting
// audio_input=file:<in.wav>, 16-bit PCM, mono or stereo, read whole at start()) is played into AudioIO::in, looped,
// so Thru tracks and the input monitor have a LINE IN; its sample rate is not converted (a mismatch is logged).
// The path may be a FIFO (tools/soak/soak-run.py reads MAIN live instead of a 600 MB/h WAV): the
// start header is written without a seek, and the final size patch (which needs one) is skipped there.
// A wake-up more than one period late (an overloaded desktop) re-anchors the clock (latePeriods(), not an xrun:
// nothing ran dry, the file just has fewer frames than the wall clock).
#pragma once

#include "platform/Platform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace platform::linux_desktop
{
struct FileAudioOptions
{
    bool allPorts = false;  // 12 WAV channels (every logical port) instead of MAIN only
    std::string inputPath;  // a 16-bit WAV played as the audio input (looped); empty = no input
};

class FileAudioDevice final : public AudioDevice
{
public:
    explicit FileAudioDevice(std::string path, double ringSeconds = 1.0, FileAudioOptions options = {})
        : path_(std::move(path)), ringSeconds_(ringSeconds), options_(std::move(options)),
          channels_(options_.allPorts ? kAllChannels : kChannels)
    {
        if (options_.allPorts)
            for (int p = 0; p < static_cast<int>(AudioPort::Count); ++p)
                ports_[static_cast<std::size_t>(p)] = {static_cast<AudioPort>(p), static_cast<std::int8_t>(2 * p),
                                                       static_cast<std::int8_t>(2 * p + 1)};
    }
    ~FileAudioDevice() override { stop(); }

    std::string_view id() const noexcept override { return "file"; }
    std::span<const PortInfo> ports() const noexcept override
    {
        return std::span<const PortInfo>(ports_.data(), options_.allPorts ? ports_.size() : 1);
    }
    bool start(const AudioConfig&, engine::AudioProcessor&, std::string& info) override; // message thread
    void stop() override;
    AudioStats stats() const noexcept override;
    std::uint64_t latePeriods() const noexcept { return late_.load(std::memory_order_relaxed); }
    std::uint64_t framesWritten() const noexcept { return written_.load(std::memory_order_relaxed); }
    const std::string& path() const noexcept { return path_; }

    static constexpr unsigned kChannels = 2;
    static constexpr unsigned kAllChannels = 2 * static_cast<unsigned>(AudioPort::Count);
    unsigned channels() const noexcept { return channels_; }
    std::size_t inputFrames() const noexcept { return inL_.size(); }

private:
    void runDevice();
    void runWriter();
    bool writeHeader(std::uint32_t dataBytes, bool rewind = true);
    bool loadInput(std::string& info); // message thread (start)

    std::string path_;
    double ringSeconds_;
    FileAudioOptions options_;
    unsigned channels_ = kChannels;
    std::array<PortInfo, static_cast<int>(AudioPort::Count)> ports_ {{{AudioPort::Main, 0, 1}}};
    std::vector<float> inL_, inR_;      // the whole input file (planar)
    std::vector<float> inPeriod_;       // one period of input, L then R (AudioIO::in)
    std::array<const float*, 2> in_ {};
    std::size_t inPos_ = 0;
    AudioConfig config_ {};
    engine::AudioProcessor* processor_ = nullptr;
    std::FILE* file_ = nullptr;
    std::vector<float> planar_;                                         // one period per written channel
    std::array<float*, 2 * static_cast<int>(AudioPort::Count)> out_ {}; // AudioIO::out (null = absent port)
    std::vector<std::int16_t> ring_;                                    // ringFrames_ interleaved frames
    std::size_t ringFrames_ = 0;
    alignas(64) std::atomic<std::uint64_t> head_ {0}; // frames produced (device thread)
    alignas(64) std::atomic<std::uint64_t> tail_ {0}; // frames consumed (writer thread)
    std::thread device_, writer_;
    std::atomic<bool> quitDevice_ {false}, quitWriter_ {false}, realtime_ {false};
    std::atomic<std::uint64_t> periods_ {0}, xruns_ {0}, late_ {0}, written_ {0};
    bool writeFailed_ = false;
};
} // namespace platform::linux_desktop
