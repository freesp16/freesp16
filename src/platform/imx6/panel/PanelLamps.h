// The panel's lamps (io::LampSink, UI-150…161) and the TX schedule. Device-independent part:
//
//   PanelLamps  (io::LampSink, message thread) — setLamp() writes the logical lamp into a TxFrame, flush() publishes it
//   LampBus     (shared) — the latest frame, the send schedule (on change, at most every kMinGapMs; heartbeat every
//               kHeartbeatMs = 4 Hz while nothing changes) and the local echo below; the panel link's reader thread
//               takes the frame to send (PanelLink.cpp)
//   local echo  — until something flushes the LampSink (the LampModel of UI-150), the panel lights itself from its
//               own input so the lamps work end to end without it: a pad hit flashes white and fades over 1 s
//               (UI-151), a held key lights white (red/green for REC/PLAY), the strip LEDs follow the strip (UI-157,
//               PITCH), the PITCH mode indicator is on (default mode). The first flush() ends the echo.
#pragma once

#include "io/controllers/ControlSurface.h"
#include "io/controllers/LampRules.h"
#include "platform/imx6/panel/AnalogBoard.h"
#include "platform/imx6/panel/PanelCodec.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace platform::imx6::panel
{
class LampBus
{
public:
    static constexpr double kHeartbeatMs = 250.0; // ≤ 4 Hz resend while nothing changes
    static constexpr double kMinGapMs = 10.0;     // changes are coalesced to ≤ 100 frames/s
    static constexpr double kEchoTickMs = 33.0;   // echo fades are recomputed at ~30 Hz while one runs

    LampBus();
    // Message thread (PanelLamps::flush): the LampModel's frame replaces the echo for good.
    void publish(const TxFrame&);
    // Reader threads: input seen by the panel or the strip, for the echo (ignored once a LampModel publishes).
    void echo(const io::ControlEvent&);
    // Sender (panel reader thread): the frame to send now, if one is due at `nowMs`. `waitMs` = when to ask again.
    bool take(double nowMs, TxFrame& out, double& waitMs);
    bool external() const noexcept { return external_.load(std::memory_order_acquire); }
    // Called after publish()/echo() so the sender wakes up (set by the sender: write to its wake pipe).
    void setWake(void (*wake)(void*), void* arg) noexcept;

private:
    void renderEcho(double nowMs); // under mutex_

    mutable std::mutex mutex_;
    TxFrame frame_;
    bool changed_ = true;
    double lastSentMs_ = -1e12;
    std::atomic<bool> external_ {false};
    // echo state
    io::LampFrame echo_ {};
    double echoAnimUntilMs_ = -1e12, lastEchoRenderMs_ = 0;
    bool echoDirty_ = false;
    void (*wake_)(void*) = nullptr;
    void* wakeArg_ = nullptr;
};

class PanelLamps final : public io::LampSink
{
public:
    // `board` (may be null): the analog filter's select line (AnalogBoard.h) puts the filter in while the unit's filter
    // is in use through the send/return loop (any route), THRU otherwise; its high-pass DAC follows afs.hpCutoff (the
    // HP CUTOFF pot or the popup). Both on change only, at flush().
    explicit PanelLamps(std::shared_ptr<LampBus> bus, std::shared_ptr<AnalogBoard> board = {})
        : bus_(std::move(bus)), board_(std::move(board))
    {
    }
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override
    {
        frame_.setLamp(c, rgb, level);
    }
    void setAnalogFilter(bool inUse, int hpCutoff) override
    {
        filterInUse_ = inUse;
        hpCutoff_ = hpCutoff;
    }
    void flush() override;

private:
    std::shared_ptr<LampBus> bus_;
    std::shared_ptr<AnalogBoard> board_;
    bool filterInUse_ = false;
    int hpCutoff_ = 0;
    TxFrame frame_;
};
} // namespace platform::imx6::panel
