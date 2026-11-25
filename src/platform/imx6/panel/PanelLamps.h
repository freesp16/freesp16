// The panel's lamps (io::LampSink, UI-150…161) and the TX schedule. Device-independent part:
//
//   PanelLamps  (io::LampSink, message thread) — setLamp() writes the logical lamp into a TxFrame, flush() publishes it
//   LampBus     (shared) — the latest frame, the send schedule (on change, at most every kMinGapMs; heartbeat every
//               kHeartbeatMs = 4 Hz while nothing changes) and the local echo below; the panel link's reader thread
//               takes the frame to send (PanelLink.cpp)
//   standby     — hardMute() (the power key, before the audio stops): every frame sent from then on carries HardMute
//               (SP-16 TX 0x66 bit 0, DJS-1000 0x6c bit 0). enterStandby() (the app's power-off,
//               Imx6Platform::enterStandby): from then on the bus sends one dark frame with HardMute and the standby
//               flag (SP-16 TX 0x67 bit 0, DJS-1000 0x6c bit 1) on the same schedule and
//               ignores publish()/echo(); the unit's power MCU (M1) then cuts the power, as after the stock app's
//               standby. The link keeps sending it (heartbeat) until then, never only once: the driver drops a frame
//               queued just before its timer stops (hardware notes, "Trap C")
//   local echo  — until something flushes the LampSink (the LampModel of UI-150), the panel lights itself from its
//               own input so the lamps work end to end without it: a pad hit flashes white and fades over 1 s
//               (UI-151), a held key lights white (red/green for REC/PLAY), the strip LEDs follow the strip (UI-157,
//               PITCH), the PITCH mode indicator is on (default mode). The first flush() ends the echo.
#pragma once

#include "core/util/PiMutex.h"
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

    explicit LampBus(Unit unit = Unit::Sp16);
    Unit unit() const noexcept { return unit_; }
    // DJS-1000 (PanelMap::muteUntilLive): its panel keeps the outputs muted until setOutputsLive(true), the app's audio
    // running, and shows its start-up state until the panel's first answer (answered()). Any thread; no effect on the
    // SP-16.
    void setOutputsLive(bool live);
    void answered();
    // Message thread (PanelLamps::flush): the LampModel's frame replaces the echo for good.
    void publish(const TxFrame&);
    // Reader threads: input seen by the panel or the strip, for the echo (ignored once a LampModel publishes).
    void echo(const io::ControlEvent&);
    // Sender (panel reader thread): the frame to send now, if one is due at `nowMs`. `waitMs` = when to ask again.
    bool take(double nowMs, TxFrame& out, double& waitMs);
    bool external() const noexcept { return external_.load(std::memory_order_acquire); }
    // Any thread: every lamp dark and the standby flag set, sent at once and then on the heartbeat until the power
    // goes; publish() and echo() are ignored from here on. False when no sender (panel link) is attached.
    bool enterStandby();
    // Any thread: HardMute on every frame from now on (sent at once). Never cleared: only the power-off sets it.
    void hardMute();
    bool standby() const noexcept { return standby_.load(std::memory_order_acquire); }
    // Called after publish()/echo() so the sender wakes up (set by the sender: write to its wake pipe).
    void setWake(void (*wake)(void*), void* arg) noexcept;

private:
    void renderEcho(double nowMs); // under mutex_
    void flagChanged(std::atomic<bool>& flag, bool value);

    mutable core::PiMutex mutex_; // the panel thread (real-time) ↔ the UI: priority inheritance
    Unit unit_;
    TxFrame frame_;
    std::atomic<bool> outputsLive_ {false}, answered_ {false};
    bool changed_ = true;
    double lastSentMs_ = -1e12;
    std::atomic<bool> external_ {false}, standby_ {false};
    bool hardMute_ = false;
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
        : bus_(std::move(bus)), board_(std::move(board)), frame_(bus_->unit())
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
