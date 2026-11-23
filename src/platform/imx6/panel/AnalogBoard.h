// The analog filter board's two digital controls (sp16-emulator docs/architecture.md "Analog board model",
// emulator/hardware_stub.c: /dev/gpiodrv and /dev/dac_spi4.0; research signal-flow.md "Analog-filter loop"):
//
//   filter select  GPIO5_IO05 "SEL_xTHRU/DSF": the vendor GPIO driver, one byte per line at offset (bank − 1)·32 +
//                  number = 133, bit 0 = the level (the app seeks, then writes). The reference drives it high while
//                  ANALOG FILTER ACTIVE is lit. Here it is high while the unit's filter is in use (ACTIVE with
//                  PROCESSOR EXTERNAL) on the MASTER route, where the filter follows the main DAC, or on a track
//                  route while the send/return loop runs (PanelLamps::setAnalogFilter); a track route without the
//                  loop runs the digital model on that track and keeps the line low, so the analog filter does not
//                  also act on the whole mix.
//   HP CUTOFF      the high-pass cutoff DAC: one 2-byte little-endian word per change of afs.hpCutoff (the pot or
//                  the popup), the 12-bit code in bits 0–11 (code 0 = the lowest cutoff, 20 Hz in the emulator's
//                  law, = the pot fully counter-clockwise = the app's afs.hpCutoff 0) and the converter's command in
//                  bits 12–15
//                  (0001 = load and update, a DAC7811-style command; the emulator ignores the nibble).
//
// LP CUTOFF and RESONANCE are analog pots on that board that no microcontroller reads: they act only on audio that
// passes through the analog filter, i.e. while the select line is high (MASTER route: engine::afs::Processor::Analog;
// track routes: the send/return loop, AlsaOutputConfig::loopDevice + Processor::External). A device that does not open
// fails once, is reported in one log line and is not retried. Both writers run on the message thread
// (PanelLamps::flush, once per UI frame, a write only on change); setEnabled / setLoop happen before the sources start.
#pragma once

#include <atomic>

namespace platform::imx6::panel
{
class AnalogBoard
{
public:
    static constexpr int kSelectLine = 4 * 32 + 5; // GPIO5_IO05
    static constexpr unsigned kDacCommand = 0x1000; // bits 12–15 = 0001

    AnalogBoard() = default;
    ~AnalogBoard();
    AnalogBoard(const AnalogBoard&) = delete;
    AnalogBoard& operator=(const AnalogBoard&) = delete;

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); } // before the sources start
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void setLoop(bool on) noexcept { loop_.store(on, std::memory_order_relaxed); } // the send/return loop is configured
    bool loop() const noexcept { return loop_.load(std::memory_order_relaxed); }
    void setActive(bool lit) noexcept;        // message thread: the filter-select line, on change only
    void setHpCutoff(int value1023) noexcept; // message thread: the DAC word, on change only

    // The 2-byte DAC word of an afs.hpCutoff value 0–1023 (also for the tests).
    static unsigned dacWord(int value1023) noexcept;

private:
    std::atomic<bool> enabled_ {false}; // Imx6Platform::configure (setting filter_board, default on)
    std::atomic<bool> loop_ {false};
    int gpioFd_ = -1, dacFd_ = -1;
    bool gpioFailed_ = false, dacFailed_ = false;
    int lastActive_ = -1, lastCode_ = -1;
};
} // namespace platform::imx6::panel
