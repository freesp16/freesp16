// The platform side of the routing (MIX-032): the port-map setting parser (imx6
// audio_map), the linux file device writing every logical port (audio_file_ports=all: MAIN stays WAV 0/1) with the real
// engine routing track 1 to OUT 3-4, and the file input stand-in (audio_input=file:…) reaching AudioIO::in, looped.
#if defined(__linux__)
#include "platform/common/audio/AlsaOutput.h"
#include "platform/linux/audio/FileAudioDevice.h"

#include "core/util/Realtime.h"
#include "tests/unit/engine/EngineTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

namespace
{
std::uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

std::vector<unsigned char> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void runFor(platform::AudioDevice& device, engine::AudioProcessor& p, const std::function<bool()>& done)
{
    std::string info;
    REQUIRE(device.start(platform::AudioConfig {}, p, info));
    INFO(info);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (! done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    device.stop();
}

class InputRecorder final : public engine::AudioProcessor // keeps the first `want` input frames (pre-allocated)
{
public:
    explicit InputRecorder(std::size_t want) : l(want), r(want) {}
    void prepare(double, int) override {}
    void process(const engine::AudioIO& io) noexcept override
    {
        if (io.numIn >= 2 && io.in != nullptr)
            for (int i = 0; i < io.frames && got < l.size(); ++i, ++got)
            {
                l[got] = io.in[0][i];
                r[got] = io.in[1][i];
            }
        calls.fetch_add(1);
    }
    std::vector<float> l, r;
    std::size_t got = 0;
    std::atomic<int> calls {0};
};
} // namespace

TEST_CASE("routing.platform: audio_map parses port:L/R lists and refuses bad syntax", "[routing][platform]")
{
    using platform::AudioPort;
    auto map = platform::common::mainOnly(12, 14);
    REQUIRE(platform::common::parsePortMap("out34:8/10,out56:4/6,out78:0/2", map));
    CHECK(map[0].left == 12);
    CHECK(map[1].port == AudioPort::Out34);
    CHECK(map[1].left == 8);
    CHECK(map[1].right == 10);
    CHECK(map[3].left == 0);
    CHECK(map[3].right == 2);
    REQUIRE(platform::common::parsePortMap("out56:-1/-1", map));
    CHECK(map[2].left == -1);
    const auto before = map;
    CHECK_FALSE(platform::common::parsePortMap("out34:8", map));
    CHECK_FALSE(platform::common::parsePortMap("bogus:1/2", map));
    CHECK_FALSE(platform::common::parsePortMap("out34:8/-1", map));
    CHECK_FALSE(platform::common::parsePortMap("out34:8/x", map));
    CHECK(map[1].left == before[1].left);
    CHECK(map[2].left == before[2].left);
}

TEST_CASE("routing.platform: the file device with every port — MAIN 0/1, OUT3-4 2/3, PHONES 8/9 (track 1 → OUT3-4)",
          "[routing][platform][linux]")
{
    const auto path = (std::filesystem::temp_directory_path() / "routing_all_ports.wav").string();
    std::filesystem::remove(path);
    platform::linux_desktop::FileAudioDevice device(path, 1.0, {true, {}});
    REQUIRE(device.ports().size() == 6);
    CHECK(device.ports()[0].left == 0);
    CHECK(device.ports()[1].left == 2);
    CHECK(device.ports()[4].port == platform::AudioPort::Phones);
    CHECK(device.ports()[4].left == 8);

    auto e = enginetest::make(); // the test tone on track 1
    const auto assign = core::builtinParams().idOf("routing.outAssign").value_or(core::ParamId {});
    REQUIRE(e->bridge().post(engine::ParamChange {assign, 0, 2.0f, 0})); // scene 1 track 1 → OUT3-4
    const auto violations = core::rt::violationCount();
    core::rt::setAbortOnViolation(false);
    runFor(device, *e, [&] { return device.stats().periods >= 60; });
    core::rt::setAbortOnViolation(true);
    CHECK(core::rt::violationCount() == violations);

    const auto wav = readFile(path);
    REQUIRE(wav.size() > 44);
    CHECK(le16(&wav[22]) == 12);
    CHECK(le16(&wav[32]) == 24);
    const std::size_t frames = le32(&wav[40]) / 24;
    REQUIRE(frames > 4096);
    std::array<int, 12> peak {};
    for (std::size_t n = 2048; n < frames; ++n) // after the assignment fade
        for (int c = 0; c < 12; ++c)
            peak[static_cast<std::size_t>(c)] = std::max(
                peak[static_cast<std::size_t>(c)],
                std::abs(static_cast<int>(static_cast<std::int16_t>(le16(&wav[44 + n * 24 + static_cast<std::size_t>(2 * c)])))));
    CHECK(peak[0] > 7900); // −12 dBFS ≈ 8190 on MAIN
    CHECK(peak[1] > 7900);
    CHECK(peak[2] > 7900); // OUT3-4 carries the track too (M + 6CH)
    CHECK(peak[3] > 7900);
    for (int c = 4; c < 8; ++c)
        CHECK(peak[static_cast<std::size_t>(c)] == 0);
    CHECK(std::abs(peak[8] - peak[0] / 2) <= 1); // PHONES: cue MASTER × HEADPHONES 100 (−6 dB)
    CHECK(peak[10] == 0);                         // FILTER SEND: silent (written by the AFS slot only)
    std::filesystem::remove(path);
}

TEST_CASE("routing.platform: the file input stand-in reaches AudioIO::in, looped (mono → both sides)",
          "[routing][platform][linux]")
{
    const auto dir = std::filesystem::temp_directory_path();
    const auto in = (dir / "routing_input.wav").string(), out = (dir / "routing_input_out.wav").string();
    {
        // 100 frames of a mono ramp, 16-bit PCM.
        std::ofstream f(in, std::ios::binary);
        const std::uint32_t frames = 100, bytes = frames * 2;
        auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4);
        u32(36 + bytes);
        f.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(1);
        u32(44100);
        u32(44100 * 2);
        u16(2);
        u16(16);
        f.write("data", 4);
        u32(bytes);
        for (std::uint32_t i = 0; i < frames; ++i)
            u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(i * 100)));
    }
    platform::linux_desktop::FileAudioDevice device(out, 1.0, {false, in});
    InputRecorder rec(1000);
    runFor(device, rec, [&] { return rec.calls.load() >= 8; });
    CHECK(device.inputFrames() == 100);
    REQUIRE(rec.got == 1000);
    for (std::size_t i = 0; i < 1000; ++i)
    {
        INFO(i);
        const float want = static_cast<float>(static_cast<int>(i % 100) * 100) / 32768.0f;
        CHECK(rec.l[i] == want);
        CHECK(rec.r[i] == want);
    }
    platform::linux_desktop::FileAudioDevice bad(out, 1.0, {false, (dir / "no-such-input.wav").string()});
    std::string info;
    InputRecorder none(1);
    CHECK_FALSE(bad.start(platform::AudioConfig {}, none, info));
    CHECK(info.find("input") != std::string::npos);
    std::filesystem::remove(in);
    std::filesystem::remove(out);
}
#endif
