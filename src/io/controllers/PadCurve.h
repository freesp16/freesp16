// Pad velocity curve (03-ui/control-surface.md UI-114; pads reported as a pressure word + a contact word, §1.1): the
// calibrated pressure word of a hit → velocity 1…127, and the word while held → pressure 0…127 (UI-115, PadPressure).
//
// Calibration ([OBS] in the sp16-emulator, panel_map.py E3/E10 and its 1.60 profile, not firmware code): a hit needs a
// contact word ≥ 60 and a pressure word above the hit threshold (169 with the reference's default calibration); the
// emulator plays velocity v as the word floor + (v − 1) · (ceiling − floor) / 126 with floor 172, ceiling 874. The
// Linear curve is the exact inverse, so velocities round-trip through the emulator unchanged.
//
// PAD CURVE Hard 3 … Linear … Soft 3, PAD THRESHOLD and PAD SENSITIVITY 1–16 (M p.42) and FIXED (Sixteen) are
// our own shapes: Hard n / Soft n = power law x^(1 + n/2) / x^(1 / (1 + n/2)) on the linear velocity; sensitivity
// scales the word above the floor by sensitivity / 8 (8 = neutral); threshold n raises the hit threshold by
// (n − 1) · 8 words (1 = calibration). The shape between floor and ceiling is unverified on hardware (no velocity
// readout on the reference).
//
// Cost: configure() builds two 1024-entry tables (message thread); velocity()/pressure() are one load each, safe on
// any thread (reader threads call them per frame).
#pragma once

#include <array>
#include <cstdint>

namespace io
{
struct PadCalibration
{
    std::uint16_t threshold = 169; // lowest pressure word that registers a hit
    std::uint16_t floor = 172;     // word of velocity 1
    std::uint16_t ceiling = 874;   // word of velocity 127 (saturation)
    std::uint16_t contactMin = 60; // contact word of a pad that is touched
};

enum class PadCurveShape : std::int8_t { Hard3 = -3, Hard2, Hard1, Linear, Soft1, Soft2, Soft3, Fixed = 100 };

struct PadResponse
{
    PadCurveShape shape = PadCurveShape::Linear;
    int threshold = 1;          // PAD THRESHOLD 1…16
    int sensitivity = 8;        // PAD SENSITIVITY 1…16 (8 = neutral)
    int fixedVelocity = 100;    // FIXED 1…127
};

class PadCurve
{
public:
    static constexpr int kWordMax = 1023; // 10-bit pressure words

    PadCurve() { configure({}, {}); }
    PadCurve(const PadCalibration& c, const PadResponse& r) { configure(c, r); }
    void configure(const PadCalibration&, const PadResponse&);

    // Velocity of a hit whose pressure word is `word`; 0 = below the threshold (no hit).
    std::uint8_t velocity(int word) const noexcept { return velocity_[clampWord(word)]; }
    // Pressure 0…127 of a held pad (aftertouch, UI-115): linear from the threshold to the ceiling.
    std::uint8_t pressure(int word) const noexcept { return pressure_[clampWord(word)]; }
    // Release hysteresis: a held pad lets go below this word (half the threshold).
    int releaseWord() const noexcept { return release_; }
    const PadCalibration& calibration() const noexcept { return calibration_; }

    // The emulator's model of a hit (panel_map.PAD_CURVE): velocity → pressure word. For tests and the bench.
    static int emulatorWord(int velocity, const PadCalibration& c = {}) noexcept;

private:
    static std::size_t clampWord(int w) noexcept
    {
        return static_cast<std::size_t>(w < 0 ? 0 : (w > kWordMax ? kWordMax : w));
    }
    PadCalibration calibration_ {};
    int release_ = 84;
    std::array<std::uint8_t, kWordMax + 1> velocity_ {}, pressure_ {};
};
} // namespace io
