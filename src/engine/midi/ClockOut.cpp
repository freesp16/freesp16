#include "engine/midi/ClockOut.h"

#include <cmath>

namespace engine::midi
{
void ClockOut::process(const BlockContext& ctx, const ClockOutSettings& s, std::uint32_t ports,
                       std::int64_t blockStart, double ppqPerFrame, const ClockOutEdges& edges, std::int64_t spp16,
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
        // The start edge: a pre-count or a lead-in ends inside the block (p0 < startPpq), or the transport started
        // at p0.
        const double startPpq = edges.startPpq > p0 ? edges.startPpq : p0;
        if (ctx.startedThisBlock && s.startStop)
        {
            const auto t = blockStart + edges.startFrame;
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
            // Tick n (PPQ n/24) belongs to the block with ⌈24·p0⌉ ≤ n < ⌈24·p1⌉: consecutive blocks share their
            // boundary bit for bit (Transport), so every tick goes out exactly once — a tick on a block boundary (a
            // transport locked to an external clock puts them there) is neither doubled nor lost. On a start edge the ticks
            // begin at the start position (never before PPQ 0) and never before the Start / Continue message.
            const bool edge = ctx.startedThisBlock;
            const double from = edge ? (startPpq < 0.0 ? 0.0 : startPpq) : p0;
            const double first = edge ? std::ceil(from * 24.0 - 1e-6) : std::ceil(from * 24.0); // start: PPQ noise
            const double end = std::ceil(p1 * 24.0);
            for (double n = first < 0.0 ? 0.0 : first; n < end; n += 1.0)
            {
                const double f = (n / 24.0 - p0) / ppqPerFrame;
                auto fr = static_cast<std::int64_t>(std::floor(f + 0.5 + 1e-6)); // + 1e-6: PPQ rounding at a half frame
                if (edge && fr < edges.startFrame)
                    fr = edges.startFrame;
                sendAll(ports, 0xF8, 0, 0, 1, blockStart + fr, out);
                ++ticks_;
            }
        }
        freePhase_ = 0.0;
        return;
    }
    if (edges.located && s.songPosition)
        spp(spp16, blockStart);
    if (s.clock && s.whileStopped && ctx.bpm > 0.0)
    {
        // SYN-021: free-running ticks at the tempo while stopped.
        const double ticksPerFrame = ctx.bpm * 24.0 / (60.0 * sampleRate_);
        const double endPhase = freePhase_ + ticksPerFrame * kBlockFrames;
        // Ticks ⌈phase⌉ ≤ n < ⌈endPhase⌉ (each tick in exactly one block, as above); the phase then drops its whole
        // ticks — exactly (endPhase − ⌊endPhase⌋), so the next block's ⌈phase⌉ continues this block's count.
        for (double n = std::ceil(freePhase_); n < std::ceil(endPhase); n += 1.0)
        {
            const auto fr = static_cast<std::int64_t>(std::floor((n - freePhase_) / ticksPerFrame + 0.5 + 1e-6));
            sendAll(ports, 0xF8, 0, 0, 1, blockStart + fr, out);
            ++ticks_;
        }
        freePhase_ = endPhase - std::floor(endPhase);
    }
}
} // namespace engine::midi
