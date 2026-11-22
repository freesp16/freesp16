// Reverb ("reverb"; spec 02-features/effects/reverb.md FX-820…828, FX-860…864, quirks FX-880…886; research
// docs/effects/send-reverb.md F121–F127, CC BY 4.0). The reference structure and laws, operation for operation as the MIT
// model tools/fx/send_reverb.py (class SendReverb, pinned commit 8901c97), with the app's own constants (FX-821 comb
// delays, FX-822 decay curve, FX-827 all-pass delays: ReverbConstants) — never the reference's firmware values.
//
//   per 64-frame block:  room target = trunc(1 + 99·k1); a new target starts the room glide (one room unit per chunk of
//                        t ms, t = max(20, 40 − ⅔·|Δ|), t itself moving ⅔ ms per step, FX-825); the comb coefficients
//                        for the current room and time (delay = base × s(room), gain = 10^(−3·d/(1000·T60)), FX-821/823)
//                        → per chunk (chunks end at room steps):
//                          pre-delay (0.1–100 ms, linear-interpolated read, 1 s glide) → × g_in(k2) → LPF 14 kHz →
//                          HPF 80 Hz → 5 parallel stereo combs (integer reads, damping LP in the loop, LR-swap spread,
//                          FX-826) → Σ → 2 series stereo all-passes (g 0.7) → wet
//   the shell (descriptor): gate mode 1, no wet high-pass, the delays' piecewise mix law with wet × 0.70795 (FX-828).
//
// Knobs k: 0 pre-delay ms = 100v (≥ 0.1), 1 room, 2 time (T60 by the FX-822 curve: log T60 linear in v between the
// anchors; g_in 0.2 → 0.1 above v = 0.5137255), 3 damping step = trunc(1 + 99(1 − v)) → 2000·9^((step − 1)/99) Hz, max
// 18 kHz (min(18 kHz, 0.4·fs) at other rates, FX-824), 4 spread, 5 mix (the shell's). No SYNC, no tempo.
//
// Quirk profile (FX-052; honoured at the next reset, the Reference profile sets every bit):
//   kReverbQuirkWarmUp       FX-880: comb and all-pass outputs muted until their rings have filled once (≈ 101 ms),
//                            then a 3 ms fade; the pre-delay's own 3 ms start fade. Sixteen: ungated, rings zeroed.
//   kReverbQuirkStaleRing    FX-881: rings kept over a reset. Sixteen: zeroed (O(1) high-water clear).
//   kReverbQuirkSharedRamp   FX-884: one ramp counter per comb parameter pair. Sixteen: independent ramps.
//   kReverbQuirkGlide100     FX-886: the comb glide time stays 100 ms until the first room change. Sixteen: the
//                            current t (40 ms after a reset).
//   kReverbQuirkGlideIn      the reset keeps the current room at the previous target and the pre-delay at its last
//                            value, so a room or pre-delay set before an activation glides in (the pre-delay over 1 s
//                            from 0.1 ms on a fresh instance). Sixteen (FX-056/FX-862): the reset snaps the
//                            combs to the stored room and the pre-delay to the stored time.
// FX-882 (pre-delay one-past-end read) is fixed in both profiles; FX-883 (blend-edge dip) cannot happen with the FX-822
// curve; FX-885 (integer comb read, zipper on room glides) is kept.
//
// Real time: prepare() allocates 5 comb rings + 2 all-pass rings + the pre-delay ring, stereo, each max(room 100 + 15 %,
// 101 ms) ≈ 4,454 frames at 44.1 kHz: 16 × 4,454 floats ≈ 285 KB (310 KB at 48 kHz); process() is noexcept, exactly 64
// frames; libm only when the time or the room changes (10 pow) and at damping steps (tan). NEON: comb and all-pass taps
// four at a time (vcvt indices, lane loads, 4-wide products/sums), filters L/R in two lanes. Budget 60k A9 cycles per
// block (tests/bench/fx/reverb.cpp). FX-864: the reverb counts kStretchQualityWeight in the stretch governor.
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/reverb_kernels.h"

#include <array>
#include <cstddef>
#include <cstdint>

#if ! defined(APP_TEST_HOOKS)
#define APP_TEST_HOOKS 1 // the device build may define 0 to drop overrideConstants()
#endif

namespace engine::fx
{
constexpr std::uint32_t kReverbQuirkWarmUp = 1u << 0;      // FX-880
constexpr std::uint32_t kReverbQuirkStaleRing = 1u << 1;   // FX-881
constexpr std::uint32_t kReverbQuirkSharedRamp = 1u << 2;  // FX-884
constexpr std::uint32_t kReverbQuirkGlide100 = 1u << 3;    // FX-886
constexpr std::uint32_t kReverbQuirkGlideIn = 1u << 4;    // reset keeps the previous room and pre-delay (our bit)

struct ReverbConstants // the app's own design (FX-821/822/827); overridable in tests only
{
    std::array<float, 5> combL{22.63f, 39.47f, 24.37f, 41.13f, 21.89f};
    std::array<float, 5> combR{38.29f, 23.71f, 40.53f, 25.19f, 24.43f};
    std::array<float, 2> apL{7.03f, 22.81f}, apR{8.19f, 21.37f}; // ms
    std::array<float, 5> t60v{0.f, 0.09f, 0.3f, 49.f / 99.f, 1.f}, t60s{0.10f, 0.87f, 3.55f, 8.70f, 100.6f};
};

class Reverb final : public EffectBase
{
public:
    static constexpr int kCombs = 5;
    static constexpr int kStretchQualityWeight = 8; // FX-864 (score = 8·reverb + active inserts + 2·players)

    void prepare(double sampleRate, int maxBlock) override; // rings for room 100 + 15 %, pre-delay ≈ 101 ms
    void reset() noexcept override;                         // FX-862 + rings zeroed (FX-881 fix)
    void setParam(int index, float value01) noexcept override; // k0 pre-delay, k1 room, k2 time, k3 damping, k4 spread
    void setOption(int which, int value) noexcept override;    // kClearState
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }
    void process(float* l, float* r, const ProcessContext&) noexcept override;

    void processScalar(float* l, float* r) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* l, float* r) noexcept;
#endif
#if APP_TEST_HOOKS
    // Structural parity test only: the comb/all-pass delays and the decay anchors used from the next reset on.
    void overrideConstants(const ReverbConstants&) noexcept;
#endif

    // The laws (also used by the tests and the panel formatter checks).
    static float t60(const ReverbConstants&, float v) noexcept; // FX-822, seconds
    static float roomScale(int room) noexcept;                  // FX-821
    static float inputGain(float v) noexcept;                   // FX-820
    static void spreadGains(float s, float (&a)[kCombs], float (&b)[kCombs]) noexcept; // FX-826
    static float dampingMaxHz(double fs) noexcept;              // FX-824

    // Introspection (tests).
    int roomCurrent() const noexcept { return roomCur_; }
    int roomTarget() const noexcept { return roomTgt_; }
    float glideMs() const noexcept { return glideT_; }
    const dsp::reverb::Comb& comb(int i) const noexcept { return combs_[i]; }
    int combRingFrames() const noexcept { return combLen_; }
    std::size_t bytes() const noexcept { return bytes_; }
    const ReverbConstants& constants() const noexcept { return k_; }

private:
    void init() noexcept;
    void slot32() noexcept;
    void slot17(bool force) noexcept;
    void slot7() noexcept;
    void slot6() noexcept;
    void initFixed() noexcept; // slot 16: fixed filters, all-pass delays and gains
    template <bool Neon> void chunk(float* l, float* r, int n) noexcept;
    template <bool Neon> void run(float* l, float* r) noexcept;

    ReverbConstants k_{};
    double fs_ = 44100.0;
    float p_[6]{};
    int roomCur_ = 50, roomSet_ = 50, roomTgt_ = 50;
    float glideT_ = 40.0f;
    int chunkN_ = 0, chunkRem_ = 0;
    bool idle_ = true;
    float inGain_ = 0.0f;
    std::uint32_t lpfStep_ = 0;
    dsp::reverb::PreDelay pd_;
    dsp::delay::SteppedIir1 lpf_, hpf_; // fixed 14 kHz / 80 Hz (one step each)
    dsp::reverb::Comb combs_[kCombs];
    dsp::reverb::AllPass aps_[2];
    int combLen_ = 0;
    std::uint32_t quirks_ = 0, active_ = 0;
    // slot-17 caches (the setters are no-ops on equal values, so skipping them is exact)
    float lastT_ = -1.0f, lastSpread_ = -1.0f;
    int lastRoom_ = -1;
    std::uint32_t lastStep_ = 0xffffffffu;
    std::size_t bytes_ = 0;
    alignas(16) float accL_[kBlockFrames]{}, accR_[kBlockFrames]{}, oL_[kBlockFrames]{}, oR_[kBlockFrames]{};
};
} // namespace engine::fx
