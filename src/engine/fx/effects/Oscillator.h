// The shared modulation oscillator of the effects, in the reference's arithmetic (spec 02-features/effects/
// oscillator.md FX-1300…1308, FX-1380; research docs/effects/oscillator.md F047, F051–F056, CC BY 4.0). Operation for
// operation equal to the MIT model tools/fx/osc.py (class Osc) with the app's own random table (FX-1305), so the
// goldens of tests/golden/oscillator match it within 1e-6. Users: Chorus, Flanger (sine, delay time), Phaser
// (triangle), and any effect that needs the reference LFO; the table/PolyBLEP oscillator of the DSP kit
// (engine/fx/dsp/Oscillator.h) stays the track LFO's audio-rate kernel.
//
//   timing    a float32 counter c += 1 per sample, wrapped at the period P = fs/f − 1 (P >= 1); f >= 0.0044100005 Hz
//   stereo    R = the same formula at c + φ·P (R leads by φ cycles)
//   waves     0 sine: double recursion y = 2cos(w)·y1 − y2, L = f32(sin w · y) clamped to ±1, renormalised about once
//               per second (the resync counter passes fs); 1 triangle (start at the peak); 2 falling saw;
//               3/4/5 pulse with duty 1/2, 1/4, 1/8; 6 random sample-and-hold of kOscRandomTable (FX-1305)
//   output    out = offset + amplitude·x, then (unless smoothing is off) y = a·out + b·y, b = exp(−2π·f_s/fs), 50 Hz
//
// The quirk adapter (FX-1380): the reference's setFrequency computes the period with a literal 44,100 Hz instead of fs.
// setQuirkProfile() with kOscQuirkPeriod44k reproduces it (Reference profile, goldens); the Sixteen profile uses fs. At
// 44.1 kHz both are identical. FX-1308: every field is initialised (the reference reads uninitialised heap for the
// resync phase, the sine counter and the random table pointer), an invalid waveform or f = 0 at init leaves the
// oscillator silent — process() writes `offset` — and never crashes.
//
// Precision (the page's allowed exception to the float32 rule): the sine recursion and its set-up are double. Real-
// time safe (no allocation, no libm in the per-sample loops; sincos/cos/acos only in init/setFrequency/resetPhase/
// resync, about once per second). Scalar: the recursion is sample-serial (≈ 1k cycles per stereo 64-frame block).
#pragma once

#include <cstdint>

namespace engine::fx
{
// Quirk bits of the oscillator (FX-052 per-effect mask; the Reference profile sets every bit).
constexpr std::uint32_t kOscQuirkPeriod44k = 1u << 0; // FX-1380

class ModOscillator
{
public:
    enum Wave : int
    {
        kSine = 0,
        kTriangle = 1,
        kSaw = 2,
        kPulse50 = 3,
        kPulse25 = 4,
        kPulse12 = 5,
        kRandom = 6
    };
    static constexpr float kMinFrequency = 0.0044100005f; // FX-1300 floor
    static constexpr int kTableSize = 8192;

    ModOscillator() noexcept { clear(); }

    // Quirk profile (FX-052/1380); kept across init().
    void setQuirkProfile(std::uint32_t mask) noexcept { quirk44k_ = (mask & kOscQuirkPeriod44k) != 0; }
    // An own table of kTableSize values (default kOscRandomTable); index kTableSize reads table[0].
    void setTable(const float* table) noexcept;

    // Reference init (0x100710): returns false (and leaves the oscillator silent) for an invalid waveform, f = 0 or
    // fs <= 0. `resyncSeed` = the reference's id word (the first renormalisation happens at (seed·448) mod fs + …);
    // the app passes 0. Smoothing is reset to 50 Hz.
    bool init(int wave, float freqHz, double fs, std::uint32_t resyncSeed = 0) noexcept;
    bool setFrequency(float hz) noexcept;    // FX-1306: keeps the phase fraction; 0 = hold
    bool setPeriodMs(float ms) noexcept;     // 1000/ms Hz
    bool setPhaseOffset(float phi) noexcept; // FX-1301: R leads by phi cycles (wrapped into [0, 1))
    bool resetPhase(float phi) noexcept;     // FX-1307
    bool setSmoothingHz(float hz) noexcept;  // <= 0: a = 1, b = 0 (no smoothing)
    void setSmoothingOff(bool off) noexcept { smoothOff_ = off; }
    void setAmplitude(float a) noexcept { amp_ = a; }
    void setOffset(float o) noexcept { off_ = o; }
    void setRandomOffset(std::uint32_t k) noexcept { rOff_ = k > kTableSize ? 0 : k; } // used by the next init()
    void setHold() noexcept { hold_ = true; }
    void clearHold() noexcept { hold_ = false; }

    // n samples of L and R (any n >= 0).
    void process(float* l, float* r, int n) noexcept;

    int wave() const noexcept { return wave_; }
    float frequency() const noexcept { return freq_; }
    float period() const noexcept { return P_; }
    float counter() const noexcept { return c_; }
    bool valid() const noexcept { return wave_ >= 0 && wave_ <= 6; }

private:
    void clear() noexcept;
    void resync() noexcept;
    float fetch() noexcept;
    void holdValues(float& l, float& r) const noexcept;
    void runSine(float* l, float* r, int n) noexcept;
    void runTriangle(float* l, float* r, int n) noexcept;
    void runSaw(float* l, float* r, int n) noexcept;
    void runPulse(float* l, float* r, int n) noexcept;
    void runRandom(float* l, float* r, int n) noexcept;

    const float* table_ = nullptr;
    float amp_ = 1.0f, off_ = 0.0f;
    int wave_ = -1;
    float freq_ = 0.0f;
    double invFs_ = 0.0, slope_ = 0.0;
    double d28_ = 0.0, d30_ = 0.0, d38_ = 0.0, d40_ = 0.0, d48_ = 0.0; // sine: cos w, 2cos w, sin w, y1, y2
    float ph_ = 0.0f, ph2_ = 0.0f;                                     // R phase offset (sine: cos, sin)
    float a_ = 0.0f, b_ = 0.0f;                                        // smoother
    float sm_[2]{};
    std::uint32_t rsCnt_ = 0, rsPer_ = 0;
    bool hold_ = false, smoothOff_ = false;
    float c_ = 0.0f, P_ = 0.0f;
    float fsf_ = 0.0f; // f32(fs) of the last successful init (the Sixteen profile's setFrequency period)
    std::uint32_t rOff_ = 0, rIdx_ = 0;
    bool quirk44k_ = false;
};
} // namespace engine::fx
