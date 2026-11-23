// MIDI clock out (spec 02-features/sync-and-transport.md SYN-020/021, NFR-004; midi.md MID-012).
// Audio thread, once per block after the transport (step 7). Sample-accurate: a tick is due at every PPQ
// n/24 of the block's [ppqStart, ppqEnd), stamped with the engine frame of that PPQ (fractional position rounded to the
// nearest frame: ≤ 11 µs at 44.1 kHz), so the stamps themselves carry no jitter; the sender's deadline sleep is the
// only jitter source (NFR-004: ≤ 0.5 ms, measured p99).
//
//   transport start   Start (FA) when it starts at PPQ 0, else Song Position (F2, when enabled) + Continue (FB), on
//                     the transport's start frame (a pre-count's end, an external clock's tick); the message precedes
//                     the first tick of that frame, and no tick of the block comes before it
//   transport stop    Stop (FC) at the block start
//   locate (stopped)  Song Position (F2) in sixteenths, when enabled
//   stopped           clock keeps running at the tempo when `clockWhileStopped` (SYN-021), free-running phase
// Song position follows the arrangement in song mode (the caller passes the position in sixteenths, ARR-008).
#pragma once

#include "engine/EngineApi.h"
#include "engine/midi/MidiOutScheduler.h"

#include <cstdint>

namespace engine::midi
{
// The transport's edges of this block (Transport::locatedThisBlock / startFrame / startPpq).
struct ClockOutEdges
{
    bool located = false;   // the position jumped
    int startFrame = 0;     // the start edge's frame (ctx.startedThisBlock)
    double startPpq = 0.0;  // the position there
};

struct ClockOutSettings
{
    bool clock = false, startStop = false, songPosition = false; // MID-012 output enables
    bool whileStopped = false;                                   // SYN-021
};

class ClockOut
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        freePhase_ = 0.0;
        ticks_ = 0;
    }
    // `ports` = output ports to send to (bit p); `blockStart` = engine frame of the block; ppqPerFrame = the
    // transport's effective rate; `spp16` = song position (sixteenths) for SPP out: where the transport starts on a
    // start edge, else at the block start.
    void process(const BlockContext& ctx, const ClockOutSettings& s, std::uint32_t ports, std::int64_t blockStart,
                 double ppqPerFrame, const ClockOutEdges& edges, std::int64_t spp16, MidiOutScheduler& out) noexcept;
    std::uint64_t ticksSent() const noexcept { return ticks_; }

private:
    void sendAll(std::uint32_t ports, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, int size, std::int64_t t,
                 MidiOutScheduler& out) noexcept
    {
        for (std::uint32_t m = ports, p = 0; m != 0; ++p, m >>= 1)
            if (m & 1u)
                out.message(static_cast<int>(p), b0, b1, b2, size, t);
    }
    double sampleRate_ = 44100.0;
    double freePhase_ = 0.0; // ticks, while stopped (SYN-021)
    std::uint64_t ticks_ = 0;
};
} // namespace engine::midi
