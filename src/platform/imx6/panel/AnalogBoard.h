// The analog (Dave Smith / Prophet-6 type) filter board's digital controls. The filter itself is an insert loop on
// the second AK4452 ("Send & Return", hw:ak4452audio,1: its playback feeds the filter input, its capture is the
// filtered return), for the MASTER route as for a track (AlsaOutputConfig::loopDevice + the AFS EXTERNAL processor).
// Hardware notes: kernel GPL sources (gpiodrv, dac_spi, board file) and sp16-emulator emulator/hardware_stub.c.
//
//   filter select  GPIO5_IO05 "SEL_xDSF/THRU" (line 133 of the vendor GPIO driver: offset (bank − 1)·32 + number,
//                  one byte, seek then write; never read(): the driver turns the line into an input first).
//                  Active low: 0 = the filter in the loop, 1 = THRU (bypass). Low while the unit's filter is in use
//                  (ANALOG FILTER ACTIVE with PROCESSOR EXTERNAL) and the loop runs; high otherwise.
//   limiter        GPIO5_IO06 "LMT_xOFF/ON" (line 134): written 1 (ON) once when the board first switches, the level
//                  the reference app sets at its start (the kernel boots it at 0); filter_limit=0 leaves it alone.
//   HP CUTOFF      the high-pass control voltage: a 12-bit multiplying DAC (DAC7811 type) on /dev/dac_spi4.0, two bytes
//                  little-endian per change of afs.hpCutoff (the pot or the popup), code in bits 0–11 (the driver puts
//                  the load-and-update command in bits 12–15 itself). Kept inside the factory range 4…3276 (80 % of
//                  full scale: the board's calibrated span, the control voltage above it is not measured): our law
//                  4 + 3272·(i/1023)³, an exponential-looking sweep in Hz like the reference's.
//
// LP CUTOFF and RESONANCE are analog pots on that board that no microcontroller reads: they act only on audio that
// passes through the filter, i.e. the loop with the select line low. A device that does not open fails once, is
// reported in one log line and is not retried. All writers run on the message thread (PanelLamps::flush, once per UI
// frame, a write only on change); setEnabled / setLoop / setLimit happen before the sources start.
#pragma once

#include <atomic>

namespace platform::imx6::panel
{
class AnalogBoard
{
public:
    static constexpr int kSelectLine = 4 * 32 + 5; // GPIO5_IO05 SEL_xDSF/THRU
    static constexpr int kLimitLine = 4 * 32 + 6;  // GPIO5_IO06 LMT_xOFF/ON
    static constexpr int kDacMin = 4, kDacMax = 3276;

    AnalogBoard() = default;
    ~AnalogBoard();
    AnalogBoard(const AnalogBoard&) = delete;
    AnalogBoard& operator=(const AnalogBoard&) = delete;

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); } // before the sources start
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void setLoop(bool on) noexcept { loop_.store(on, std::memory_order_relaxed); } // the send/return loop is configured
    bool loop() const noexcept { return loop_.load(std::memory_order_relaxed); }
    void setLimit(bool on) noexcept { limit_.store(on, std::memory_order_relaxed); } // write LMT ON at the first switch
    void setFilterIn(bool in) noexcept;       // message thread: the select line (0 = filter in), on change only
    void setHpCutoff(int value1023) noexcept; // message thread: the DAC code, on change only

    // The 12-bit DAC code of an afs.hpCutoff value 0–1023 (kDacMin…kDacMax; also for the tests).
    static unsigned dacCode(int value1023) noexcept;
    // The select line's level for "filter in" (active low).
    static constexpr unsigned char selectLevel(bool filterIn) noexcept { return filterIn ? 0 : 1; }

private:
    bool writeLine(int line, unsigned char level, const char* what) noexcept;
    std::atomic<bool> enabled_ {false}; // Imx6Platform::configure (setting filter_board, default on)
    std::atomic<bool> loop_ {false};
    std::atomic<bool> limit_ {true};
    int gpioFd_ = -1, dacFd_ = -1;
    bool gpioFailed_ = false, dacFailed_ = false, limitDone_ = false;
    int lastIn_ = -1, lastCode_ = -1;
};
} // namespace platform::imx6::panel
