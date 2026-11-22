#include "engine/midi/ClockOut.h"

#include <cmath>

namespace engine::midi
{
void ClockOut::process(const BlockContext& ctx, const ClockOutSettings& s, std::uint32_t ports,
                       std::int64_t blockStart, double ppqPerFrame, bool located, std::int64_t spp16,
                       MidiOutScheduler& out) noexcept
{
    if (ports == 0)
        return;
    const auto spp = [&](std::int64_t pos16, std::int64_t t) {
        const auto v = static_cast<int>(pos16 < 0 ? 0 : (pos16 > 16383 ? 16383 : pos16));
        sendAll(ports, 0xF2, static_cast<std::uint8_t>(v & 127), static_cast<std::uint8_t>(v >> 7), 3, t, out);
    };
    if (ctx.stoppedThisBlock && s.startStop)
        sendAll(ports, 0xFC, 0, 0, 1, blockStart, out);
    if (ctx.playing)
    {
        const double p0 = ctx.ppqStart, p1 = ctx.ppqEnd;
        // The start frame: a pre-count ends inside the block (p0 < 0 ≤ p1) or the transport started at p0.
        if (ctx.startedThisBlock && s.startStop)
        {
            const double startPpq = p0 < 0.0 ? 0.0 : p0;
            const auto t = blockStart + (p0 < 0.0 && ppqPerFrame > 0.0
                                             ? static_cast<std::int64_t>(std::ceil(-p0 / ppqPerFrame - 1e-9))
                                             : 0);
            if (startPpq < 1e-9)
                sendAll(ports, 0xFA, 0, 0, 1, t, out);
            else
            {
                if (s.songPosition)
                    spp(spp16, t);
                sendAll(ports, 0xFB, 0, 0, 1, t, out);
            }
        }
        if (s.clock && ppqPerFrame > 0.0 && p1 > p0)
        {
            // Ticks n/24 in [max(p0, 0), p1).
            const double from = p0 < 0.0 ? 0.0 : p0;
            double n = std::ceil(from * 24.0 - 1e-9);
            for (int guard = 0; guard < 64; ++guard, n += 1.0)
            {
                const double q = n / 24.0;
                if (q >= p1)
                    break;
                const double f = (q - p0) / ppqPerFrame;
                const auto fr = static_cast<std::int64_t>(std::floor(f + 0.5 + 1e-6)); // + 1e-6: PPQ rounding at an exact half frame
                sendAll(ports, 0xF8, 0, 0, 1, blockStart + fr, out);
                ++ticks_;
            }
        }
        freePhase_ = 0.0;
        return;
    }
    if (located && s.songPosition)
        spp(spp16, blockStart);
    if (s.clock && s.whileStopped && ctx.bpm > 0.0)
    {
        // SYN-021: free-running ticks at the tempo while stopped.
        const double ticksPerFrame = ctx.bpm * 24.0 / (60.0 * sampleRate_);
        const double endPhase = freePhase_ + ticksPerFrame * kBlockFrames;
        double n = std::ceil(freePhase_ - 1e-9);
        for (int guard = 0; guard < 64 && n < endPhase; ++guard, n += 1.0)
        {
            const auto fr = static_cast<std::int64_t>(std::floor((n - freePhase_) / ticksPerFrame + 0.5 + 1e-6));
            sendAll(ports, 0xF8, 0, 0, 1, blockStart + fr, out);
            ++ticks_;
        }
        freePhase_ = endPhase - std::floor(endPhase); // in [0, 1): a tick exactly on the boundary is the next block's
    }
}
} // namespace engine::midi
