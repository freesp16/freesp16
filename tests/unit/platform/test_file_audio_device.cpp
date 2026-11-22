// The linux platform's `file` audio device: a known tone from a fake AudioProcessor ends up in the WAV with a
// correct RIFF header, the device thread runs under the real-time contract (no allocation: APP_RT_CHECKS hook), and
// the period/xrun statistics add up.
#if defined(__linux__)
#include "platform/linux/audio/FileAudioDevice.h"

#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace
{
// 440 Hz at 0.25 (−12 dBFS) on MAIN L, 0.125 on MAIN R, phase from a frame counter (deterministic content).
class ToneProcessor final : public engine::AudioProcessor
{
public:
    void prepare(double sampleRate, int) override { rate = sampleRate; }
    void process(const engine::AudioIO& io) noexcept override
    {
        realtime = realtime && core::rt::isRealtimeThread();
        for (int i = 0; i < io.frames; ++i)
        {
            const double s = std::sin(2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(frame + i) / rate);
            io.out[0][i] = static_cast<float>(0.25 * s);
            io.out[1][i] = static_cast<float>(0.125 * s);
        }
        frame += io.frames;
        calls.fetch_add(1);
    }
    double rate = 44100;
    long frame = 0;
    bool realtime = true;
    std::atomic<int> calls {0};
};

std::uint32_t le32(const unsigned char* p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::int16_t expected(double amp, long n)
{
    const double w = 2.0 * 3.14159265358979323846 * 440.0 / 44100.0;
    const float x = static_cast<float>(amp * std::sin(w * static_cast<double>(n)));
    const float s = x * 32767.0f;
    return static_cast<std::int16_t>(static_cast<int>(s + (s >= 0.0f ? 0.5f : -0.5f)));
}
} // namespace

TEST_CASE("platform file device: WAV header, MAIN content, real-time thread, stats", "[platform][linux]")
{
    const auto path = (std::filesystem::temp_directory_path() / "file_device_test.wav").string();
    std::filesystem::remove(path);
    platform::linux_desktop::FileAudioDevice device(path);
    CHECK(device.id() == "file");
    REQUIRE(device.ports().size() == 1);
    CHECK(device.ports()[0].port == platform::AudioPort::Main);
    CHECK(device.ports()[0].left == 0);
    CHECK(device.ports()[0].right == 1);

    ToneProcessor tone;
    std::string info;
    const auto violations = core::rt::violationCount();
    core::rt::setAbortOnViolation(false); // count instead of abort, so a failure is reported here
    REQUIRE(device.start(platform::AudioConfig {}, tone, info));
    INFO(info);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tone.calls.load() < 40 && std::chrono::steady_clock::now() < deadline) // ≈ 0.23 s of audio
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    device.stop();
    core::rt::setAbortOnViolation(true);
    CHECK(core::rt::violationCount() == violations); // no allocation on the device thread
    CHECK(tone.realtime);                           // process() ran on a real-time-marked thread

    const auto stats = device.stats();
    CHECK(stats.periods >= 40);
    CHECK(stats.periods == static_cast<std::uint64_t>(tone.calls.load()));
    CHECK(stats.xruns == 0);
    CHECK(stats.sampleRate == 44100);
    CHECK(stats.periodFrames == 256);
    CHECK(device.framesWritten() == stats.periods * 256);

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> wav((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(wav.size() > 44);
    CHECK(std::memcmp(wav.data(), "RIFF", 4) == 0);
    CHECK(le32(&wav[4]) == wav.size() - 8);
    CHECK(std::memcmp(&wav[8], "WAVEfmt ", 8) == 0);
    CHECK(le32(&wav[16]) == 16);
    CHECK(le16(&wav[20]) == 1); // PCM
    CHECK(le16(&wav[22]) == 2);
    CHECK(le32(&wav[24]) == 44100);
    CHECK(le32(&wav[28]) == 44100 * 4);
    CHECK(le16(&wav[32]) == 4);
    CHECK(le16(&wav[34]) == 16);
    CHECK(std::memcmp(&wav[36], "data", 4) == 0);
    const auto dataBytes = le32(&wav[40]);
    CHECK(dataBytes == wav.size() - 44);
    CHECK(dataBytes == device.framesWritten() * 4);

    // Every frame is the processor's output, in order (no period dropped or repeated).
    const long frames = static_cast<long>(dataBytes / 4);
    long mismatches = 0;
    int peak = 0;
    for (long n = 0; n < frames; ++n)
    {
        const auto* f = &wav[44 + static_cast<std::size_t>(n) * 4];
        const auto l = static_cast<std::int16_t>(le16(f)), r = static_cast<std::int16_t>(le16(f + 2));
        mismatches += std::abs(l - expected(0.25, n)) > 1 || std::abs(r - expected(0.125, n)) > 1;
        peak = std::max(peak, std::abs(static_cast<int>(l)));
    }
    CHECK(mismatches == 0);
    CHECK(std::abs(20.0 * std::log10(peak / 32768.0) - (-12.04)) < 0.1); // −12 dBFS peak on MAIN L
    std::filesystem::remove(path);
}

TEST_CASE("platform file device: an unwritable path fails start() with a reason", "[platform][linux]")
{
    platform::linux_desktop::FileAudioDevice device("/nonexistent-dir/x.wav");
    ToneProcessor tone;
    std::string info;
    CHECK_FALSE(device.start(platform::AudioConfig {}, tone, info));
    CHECK(info.find("cannot create") != std::string::npos);
    device.stop(); // harmless
}
#endif
