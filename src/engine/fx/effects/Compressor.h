// Compressor (spec 02-features/effects/compressor.md FX-1120…1183; research docs/effects/compressor.md,
// CC BY 4.0). A feed-forward, stereo-linked, block-detecting compressor, operation for operation the MIT model
// tools/fx/compressor.py (`Compressor.process` for the Insert, `process_send` for Send/Master), with std:: maths in
// double rounded to float where the model calls glibc expf / log10 and fdlibm powf (the default
// build; the reference libm is only reachable with tables taken from a reference unit, see tests/golden/compressor).
//
// Per 64-frame block (in place, wet only; the shell mixes dry 1 − v and wet v·makeup, FX-1124, EffectDefined law):
//   1. look-ahead line (Send/Master, and Insert with FX-1163): an 88-sample (2 ms at 44.1 kHz) delay per channel and a
//      1/147-per-sample linear crossfade towards the delayed signal while Look-ahead is on (FX-1125)
//   2. input meters (FX-1126, display only)
//   3. detector on the UNDELAYED input: peak max(|L|, |R|), or RMS over a 3 ms ring of max(L², R²) summed in double
//      (the page defines double here), √2·√max mean; floor 1e-8; 1-based position of the maximum
//   4. gain computer in dB (interpolated slope/point form of the quadratic soft knee, FX-1121), target tracking with
//      +3 dB hysteresis, hold min(release/4, 10 ms), ×e per block release (FX-1122)
//   5. one pow per block: k = (target/gain)^(1/ramp); per sample gain ·= k clamped at the target, then the slew
//      limits a = exp(−1/(attack·fs)), r = exp(+1/(release·fs)) (FX-1123); out = gain·in (vectorised)
//   6. output meters; dry = 1 − v, wet = v·makeup reported through telemetry(kTelemetryDry/Wet)
// Parameter-derived constants (T, 1/R, K, makeup, a, r, hold lengths) are recomputed only when a parameter changed.
//
// Parameters (internal index k, FX-060): k0 Threshold q(−52, 1) dB, k1 Ratio q(10, 301)/10, k2 Attack q(1, 1020)
// (0.05…1 ms, then 2…1000 ms), k3 Release q(10, 4001) ms, k4 Gain q(0, 51) dB, k5 Mix. Options (setOption codes = the
// positions in the descriptor's option list, kCompressorOptions): 0 Look-ahead (Send/Master; Insert behind the feature
// flag fx.compressor.insertLookAhead, FX-1163, off by default), 1 Detect (type2: 0 Peak, 1 RMS), 2 Knee (type1: 0
// Hard, 1 Soft = 18 dB). Every slot kind runs the same code: with Look-ahead off the crossfade rests on the undelayed
// side, which equals the reference Insert routine on finite input.
//
// Quirks (FX-052/053): FX-1180 (Insert late start) is the shell's skip/clamp (ShellPolicy skipWhenSilent/clampOutput,
// kQuirkShellSkipClamp); kQuirkHoldInit reproduces FX-1183 (the hold length starts at 0; not audible: the hold only
// counts after an attack, which sets it). FX-1181 is a model/import matter (FX-082), FX-1182 is unreachable (FX-057).
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/Numerics.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::fx::effects
{
// The descriptor's option list (setOption code = position; UID-827 cells: 1 LOOK AHEAD, 2 PEAK | RMS, 3 SOFT KNEE).
// The keys type1/type2/lookAhead are the model fields (core::model::EffectSlot) the bridge posts (ModelFxSlots).
inline constexpr std::string_view kCompressorOptions[] = {"fx.compressor.lookAhead", "fx.compressor.type2",
                                                          "fx.compressor.type1"};
inline constexpr std::string_view kCompressorInsertLookAheadFlag = "fx.compressor.insertLookAhead"; // FX-1163

class Compressor final : public EffectBase
{
public:
    enum Option : int
    {
        kOptLookAhead = 0, // "fx.compressor.lookAhead"
        kOptDetect = 1,    // "fx.compressor.type2": 0 Peak, 1 RMS
        kOptKnee = 2       // "fx.compressor.type1": 0 Hard, 1 Soft
    };
    enum Telemetry : int
    {
        kTelGainReduction = 0, // the block's gain reduction, 0 = none … 1 = kGrFullScaleDb (the GR bar, UID-834)
        kTelOutput = 1,        // output peak-hold meter × makeup, capped at 2 (FX-1126)
        kTelInputL = 2,
        kTelInputR = 3,
        kTelMinGain = 4 // the block's minimum gain, linear
    };
    static constexpr std::uint32_t kQuirkHoldInit = 1u << 0; // FX-1183 (per-effect bit of the quirk mask)
    static constexpr float kGrFullScaleDb = 24.0f;           // full scale of the GR bar (our choice)
    static constexpr int kMaxOptions = 3;

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-1161
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kOpt*, kClearState
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    int latencySamples() const noexcept override; // the look-ahead delay (88 at 44.1 kHz) while Look-ahead is on
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirkHoldInit_ = (mask & kQuirkHoldInit) != 0; }
    float telemetry(int which) const noexcept override;

    // The two implementations (bit-exact on finite input; process() runs NEON on the device).
    void processScalar(float* left, float* right) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* left, float* right) noexcept;
#endif

    // Test access (state after the last block).
    float gain() const noexcept { return gain_; }
    float targetGain() const noexcept { return target_; }
    float levelDb() const noexcept { return levelDb_; }
    float makeup() const noexcept { return makeup_; }
    int rampLength() const noexcept { return ramp_; }
    int holdLimit() const noexcept { return holdLim_; }
    int lookAheadDelay() const noexcept { return laLen_ - 1; }
    int rmsWindow() const noexcept { return win_; }

private:
    struct Meter // FX-1126 peak hold (display only): floor 1e-8, ceiling 2, −1 dB per block, hold 6 blocks
    {
        float cur = 1e-8f;
        int cnt = 0, hold = 6;
        void update(float peak) noexcept;
        void reset() noexcept;
    };

    template <bool Neon> void run(float* left, float* right) noexcept;
    template <bool Neon> bool lookAhead(const float* l, const float* r) noexcept; // true: the path is aL_/aR_
    void detectRms(const float* l, const float* r) noexcept;
    void updateConstants() noexcept;
    void gainComputer() noexcept; // FX-1121/1122: fills gbuf_
    void clear() noexcept;        // the freshly prepared state

    // parameters (stored step fractions) and options
    float p_[6]{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float knee_ = 0.0f; // type1 as stored (0 / 1)
    int detect_ = 0;    // type2 (1 = RMS)
    bool lookAheadParam_ = false, lookAhead_ = false;
    bool dirty_ = true, quirkHoldInit_ = false;

    // derived constants (updateConstants)
    float thr_ = 0.0f, invRatio_ = 1.0f, kneeDb_ = 0.0f, makeupTarget_ = 1.0f, a_ = 0.0f, r_ = 0.0f;
    int r1_ = 441, r2_ = 0, ms44_ = 44;

    // gain computer state (compressor.py field names in comments)
    float slope_ = 1.0f, thrPt_ = 0.0f; // +0x68c / +0x690
    float gain_ = 1.0f, makeup_ = 1.0f; // +0x694 / +0x698
    float peak_ = 0.0f;                 // +0x6a4
    int ramp_ = 0;                      // +0x6a8
    float minGain_ = 1.0f;              // +0x6dc
    float levelDb_ = 0.0f;              // +0x750
    int hold_ = 0, holdLim_ = 0;        // +0x754 / +0x758
    float target_ = 1.0f;               // +0x75c
    float lastKnee_ = 0.0f, lastThr_ = 1.0f, lastRatio_ = 0.0f;
    float dry_ = 1.0f, wet_ = 0.0f; // EffectBase +0x10 / +0x14
    float e_ = 0.0f, einv_ = 0.0f;  // expf(1), expf(−1)

    // RMS detector: ring of trunc(3 ms · fs) values, double sum
    std::vector<float> ring_;
    int win_ = 132, pos_ = 0, cnt_ = 0;
    float invWin_ = 1.0f / 132.0f;
    double sum_ = 0.0;

    // look-ahead line: laLen_ = trunc(2 ms · fs) + 1 slots per channel
    std::vector<float> laL_, laR_;
    int laLen_ = 89, laPos_ = 0;
    float laFade_ = 1.0f, laStep_ = 0.0f;

    Meter mtr_[4]{}; // in L, in R, out L, out R
    double fs_ = 44100.0;

    alignas(16) float gbuf_[kBlockFrames]{};
    alignas(16) float aL_[kBlockFrames]{};
    alignas(16) float aR_[kBlockFrames]{};
    alignas(16) float fbuf_[kBlockFrames]{};
    alignas(16) float cbuf_[kBlockFrames]{};
};
} // namespace engine::fx::effects
