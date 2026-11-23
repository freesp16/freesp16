// The hardware panel link (the imx6 platform's "panel" io::ControlSource, and the sender of its lamp
// frames): /dev/subucom_spi0.1, 128-byte RX frames in, 128-byte TX lamp frames out.
//
// Sources (emulator, not firmware): sp16-emulator/emulator/panel_map.py (E1-E11 frame layout),
// hardware_stub.c (the ioctls the panel driver accepts: word size, byte order, transfer size, interval, scan timer,
// and the 16-byte SPI transfer descriptor of request 0x40107000 that carries a TX frame: multi 0, part 0 of 1),
// protocol.py. The layout itself is PanelRoles.h / resources/controllers/panel.json, the decoding PanelCodec.cpp.
//
// Threads: one reader thread (normal priority, never the audio thread) polls the link and a wake pipe. Per RX frame it
// decodes the events, pushes pad hits straight into the engine's TriggerSink (UI-118) and queues every event into an
// SPSC queue (reader → UI thread) that poll() drains; between frames it sends the TX frame the LampBus says is due
// (on change, heartbeat ≤ 4 Hz). Only one process may own the link: this replaces the stock application.
#pragma once

#include "core/util/SpscQueue.h"
#include "io/controllers/ControlSurface.h"
#include "platform/imx6/panel/PanelCodec.h"
#include "platform/imx6/panel/PanelLamps.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace platform::imx6
{
struct PanelStats
{
    long frames = 0, badCrc = 0, shortReads = 0, txFrames = 0, txErrors = 0, dropped = 0;
    bool running = false;
};

class PanelLink final : public io::ControlSource
{
public:
    PanelLink(engine::TriggerSink* triggers, std::shared_ptr<panel::LampBus> lamps, bool trace);
    ~PanelLink() override;
    std::string_view id() const override { return "panel"; }
    bool start(std::string& info) override; // opens and configures the link, starts the reader thread
    void stop() override;
    void poll(std::vector<io::ControlEvent>& out) override; // UI thread, wait-free
    PanelStats stats() const;

private:
    void run();
    void sendDue(double nowMs, double& waitMs);
    static void wake(void* self);

    engine::TriggerSink* triggers_;
    std::shared_ptr<panel::LampBus> lamps_;
    bool trace_;
    panel::PanelDecoder decoder_;
    io::PadCalibration padBase_ {}; // the calibration UTILITY's pad preferences apply over (PanelPads.h)
    std::uint32_t padPrefsSeen_ = 0;
    panel::EventBatch batch_;
    core::SpscQueue<io::ControlEvent, 1024> queue_;
    int fd_ = -1, wakeRead_ = -1, wakeWrite_ = -1;
    std::thread thread_;
    std::atomic<bool> quit_ {false}, running_ {false};
    std::atomic<long> frames_ {0}, badCrc_ {0}, shortReads_ {0}, txFrames_ {0}, txErrors_ {0}, dropped_ {0};
};
} // namespace platform::imx6
