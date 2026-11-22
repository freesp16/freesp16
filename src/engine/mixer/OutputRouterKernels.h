// Block kernels of the output router, the headphone router and the input monitor. Every
// kernel has a scalar reference (`scalar::`) and a NEON version (`neon::`, ARMv7 intrinsics, only where __ARM_NEON is
// defined); tests/unit/routing/test_routing_kernels.cpp compares them bit for bit (they skip on x86, where only the
// scalar reference exists; scripts/neon-tests.sh runs them under qemu-arm). The unqualified functions
// dispatch at compile time.
//
// Arithmetic: one rounding per operation, no fused multiply-add (the file is compiled with fp-contract off), and only
// operations whose result does not depend on association: mono = (l + r) × 0.5 (× 0.5 is exact), then × g, then
// dst + that. So the device's -funsafe-math-optimizations cannot make the scalar reference differ from NEON.
// Contract: n is a multiple of 4 (the engine block is 64); pointers 16-byte aligned preferred, not required.
#pragma once

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
 #define APP_ROUTING_HAVE_NEON 1
#else
 #define APP_ROUTING_HAVE_NEON 0
#endif

namespace engine::mixer::routing
{
namespace scalar
{
void add(float* dst, const float* src, int n) noexcept;                                  // dst += src
void addGain(float* dst, const float* src, const float* g, int n) noexcept;              // dst += src × g[i]
void addMono(float* dst, const float* l, const float* r, int n) noexcept;                // dst += (l + r) × 0.5
void addMonoGain(float* dst, const float* l, const float* r, const float* g, int n) noexcept; // … × g[i]
void copyGain(float* dst, const float* src, const float* g, int n) noexcept;             // dst = src × g[i]
void copyScale(float* dst, const float* src, float g, int n) noexcept;                   // dst = src × g
void multiply(float* dst, const float* g, int n) noexcept;                               // dst ×= g[i]
} // namespace scalar

#if APP_ROUTING_HAVE_NEON
namespace neon
{
void add(float* dst, const float* src, int n) noexcept;
void addGain(float* dst, const float* src, const float* g, int n) noexcept;
void addMono(float* dst, const float* l, const float* r, int n) noexcept;
void addMonoGain(float* dst, const float* l, const float* r, const float* g, int n) noexcept;
void copyGain(float* dst, const float* src, const float* g, int n) noexcept;
void copyScale(float* dst, const float* src, float g, int n) noexcept;
void multiply(float* dst, const float* g, int n) noexcept;
} // namespace neon
namespace active = neon;
#else
namespace active = scalar;
#endif

inline void add(float* d, const float* s, int n) noexcept { active::add(d, s, n); }
inline void addGain(float* d, const float* s, const float* g, int n) noexcept { active::addGain(d, s, g, n); }
inline void addMono(float* d, const float* l, const float* r, int n) noexcept { active::addMono(d, l, r, n); }
inline void addMonoGain(float* d, const float* l, const float* r, const float* g, int n) noexcept
{
    active::addMonoGain(d, l, r, g, n);
}
inline void copyGain(float* d, const float* s, const float* g, int n) noexcept { active::copyGain(d, s, g, n); }
inline void copyScale(float* d, const float* s, float g, int n) noexcept { active::copyScale(d, s, g, n); }
inline void multiply(float* d, const float* g, int n) noexcept { active::multiply(d, g, n); }
constexpr bool hasNeon() noexcept { return APP_ROUTING_HAVE_NEON != 0; }

// The 128-sample selector fade of MIX-033 / MIX-042: the research Selector's GainSmoother (tools/fx/headphone_mix.py
// Selector = track_mix.Smoother(128, 1.0), F209/F212), operation for operation. setTarget() starts a linear ramp from the
// gain reached to the target over 128 frames (step = (target − gain) × (1/128), gain += step per frame); the same
// target again changes nothing. From 1, the fade-out frames are (128 − k) / 128 (the last one 1/128); from 0, the
// fade-in frames are k / 128 (the first one 0); both exact in float. fill() writes the block's gains (null: advance).
class SelectorFade
{
public:
    static constexpr int kLength = 128;
    static constexpr float kInv = 1.0f / static_cast<float>(kLength); // exact

    void reset(float g) noexcept
    {
        cur_ = target_ = g;
        step_ = 0.0f;
        count_ = 0;
    }
    void setTarget(float t) noexcept
    {
        if (target_ == t)
            return;
        const float diff = t - cur_;
        step_ = diff * kInv;
        target_ = t;
        count_ = kLength;
    }
    bool idle() const noexcept { return count_ == 0; }
    float current() const noexcept { return cur_; }
    float target() const noexcept { return target_; }
    void fill(float* out, int n) noexcept;

private:
    float cur_ = 1.0f, target_ = 1.0f, step_ = 0.0f;
    int count_ = 0;
};
} // namespace engine::mixer::routing
