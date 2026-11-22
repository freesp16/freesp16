// Controller parse budget: bench_controller_parse ≤ 2 µs per message on the host
// — one MIDI message through MidiControlSource::onMidi (table lookup, encoding, event queue, TriggerNow for pad hits),
// a mixed Generic 16 + 8 stream (pads, aftertouch, encoders, buttons, unbound clock). The device figure (< 1 % of a
// core at 3,000 messages/s, i.e. ≤ 3.3 µs per message on the Cortex-A9) is measured on hardware; desktop timings
// are relative. Also the lamp flush of a full 16-pad refresh.
#include "io/controllers/MidiControlSource.h"
#include "io/controllers/profiles/Profile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
struct NullSink final : engine::TriggerSink
{
    std::uint64_t n = 0;
    bool push(const engine::TriggerNow&) noexcept override
    {
        ++n;
        return true;
    }
};
} // namespace

TEST_CASE("bench_controller_parse: one message through the controller source", "[bench][controllers]")
{
    namespace ctl = io::controllers;
    NullSink sink;
    ctl::MidiControlSource src(ctl::builtinProfiles()[0], 0, "BENCH", &sink);
    std::vector<std::array<std::uint8_t, 3>> stream;
    for (int i = 0; i < 64; ++i)
    {
        stream.push_back({0x99, static_cast<std::uint8_t>(36 + i % 16), static_cast<std::uint8_t>(40 + i)});
        stream.push_back({0xA9, static_cast<std::uint8_t>(36 + i % 16), static_cast<std::uint8_t>(i)});
        stream.push_back({0xB9, static_cast<std::uint8_t>(70 + i % 6), static_cast<std::uint8_t>(i % 2 ? 1 : 127)});
        stream.push_back({0x89, static_cast<std::uint8_t>(36 + i % 16), 0});
        stream.push_back({0xF8, 0, 0});
    }
    std::vector<io::ControlEvent> out;
    out.reserve(1024);
    constexpr int kRounds = 4000;
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t messages = 0;
    for (int r = 0; r < kRounds; ++r)
    {
        for (const auto& m : stream)
            src.onMidi(m.data(), m[0] >= 0xF8 ? 1 : 3, 0.0);
        messages += stream.size();
        out.clear();
        src.poll(out); // the UI side drains the queue between bursts (≤ 512 events)
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()
                    / static_cast<double>(messages);
    std::printf("BENCH controller_parse %.3f us/message (%llu messages, %llu triggers; limit 2 us host)\n", us,
                static_cast<unsigned long long>(messages), static_cast<unsigned long long>(sink.n));
    CHECK(us <= 2.0);
    CHECK(src.stats().dropped == 0);

    // A full pad refresh: 16 colour changes → 16 note-ons, every flush ≥ 15 ms apart.
    std::uint64_t sent = 0;
    auto send = [&](const std::uint8_t*, int) {
        ++sent;
        return true;
    };
    src.setSend(send);
    const auto l0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 1000; ++f)
    {
        for (int p = 1; p <= 16; ++p)
            src.setLamp(io::pad(p), (f + p) % 2 ? 0xFF7E72u : 0x3FB3FFu, 255);
        src.flushAt(f * 16.7);
    }
    const double lampUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - l0).count() / 1000;
    std::printf("BENCH controller_lamps %.2f us per 16-pad refresh (%llu messages)\n", lampUs,
                static_cast<unsigned long long>(sent));
    CHECK(sent >= 16000);
}
