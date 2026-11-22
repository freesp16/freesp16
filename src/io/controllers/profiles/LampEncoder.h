// Lamp feedback of a controller profile (control-surface.md UI-135, Part 3): the LampModel's resolved
// lamps (LampSink::setLamp: colour + level) → the profile's lamp messages. Each lamp's colour, scaled by its level, is
// matched to the nearest entry of the lamp's palette (squared RGB distance); the entry's value is sent as a note-on
// velocity or a CC value on the lamp's note / CC. Only lamps whose value changed are sent, and at most every
// kMinFlushMs (≈ 60 Hz), so a full 16-pad refresh is 48 bytes ≈ 15 ms of DIN bandwidth at worst and nothing at all
// while the lamps hold still. Message thread only; no allocation after construction.
#pragma once

#include "io/controllers/ControlNames.h"
#include "io/controllers/profiles/Profile.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace io::controllers
{
class LampEncoder
{
public:
    static constexpr double kMinFlushMs = 15.0; // ≤ 60 sends per second (one per 60 Hz frame, with jitter allowed)

    explicit LampEncoder(const LoadedProfile&);
    LampEncoder(const LampEncoder&) = delete; // lamps point into palettes_
    LampEncoder& operator= (const LampEncoder&) = delete;
    bool empty() const noexcept { return lamps_.empty(); }

    void set(Control, std::uint32_t rgb, std::uint8_t level) noexcept;
    // Sends every changed lamp through `send` (3-byte messages); returns the number sent. Nothing when called again
    // within kMinFlushMs (the changes wait for the next call). A failed send is retried next time.
    using Send = std::function<bool(const std::uint8_t* bytes, int size)>;
    int flush(double nowMs, const Send& send);
    void resend() noexcept; // the next flush sends every lamp (a reconnected output)

    // The palette entry nearest to `rgb` (0xRRGGBB): its index.
    static int nearest(std::span<const std::uint32_t> palette, std::uint32_t rgb) noexcept;
    // The value a lamp would send now (tests); −1 for a control without a lamp.
    int value(Control) const noexcept;

private:
    struct Lamp
    {
        std::array<std::uint8_t, 2> head; // status byte (with channel), number
        const std::vector<std::uint32_t>* palette;
        std::uint8_t want = 0;
        std::int16_t sent = -1;
    };
    std::vector<Lamp> lamps_;
    std::array<std::int16_t, kLampSlots> index_ {};
    std::vector<std::vector<std::uint32_t>> palettes_;
    double lastFlushMs_ = -1.0e18;
};
} // namespace io::controllers
