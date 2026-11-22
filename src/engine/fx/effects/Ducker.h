// Ducker (spec 02-features/effects/ducker.md FX-1210…1282; research docs/effects/ducker.md, CC BY 4.0).
// A sequencer-triggered gain envelope with NO audio key: on every sounding trigger of the Source track the gain falls
// from 1 to 1 − Depth over the attack and rises back to 1 over the release, along the curves c = 100^(2v − 1).
// Operation for operation the MIT model tools/fx/ducker.py (slot 25 `process`, the update 0xf6630, the attack and
// release segments, the retrigger re-entry), with std::pow / std::log10 in double rounded to float where the model
// uses its correctly rounded pow and its fdlibm log10f port (goldens at 1e-6).
//
// Per 64-frame block (in place, wet only; shell: dry 0 / wet 1, gate 0, no HPF):
//   host rule (FX-1242/1243, F201): TRIGGER = ctx.triggerMask bit of Source, OFFSET = ctx.triggerOffset[Source],
//   BPM = ctx.bpm (the processor clamps it to 1…999, FX-049/1244); a MIDI-track Source never ducks (FX-1241) unless
//   the FX-1263 option kOptFollowMidi is set. The engine's TriggerRouter sets the mask only for Full/Half hits of an
//   unmuted track (FX-1240/1241) and the engine clears it every block.
//   update → state machine idle / attack / release (FX-1220…1224) → the gain trace of the block (scalar: the trace is
//   a serial recurrence) → out = g·in on both channels (vectorised). An idle block returns at once (FX-1260).
//
// Parameters (k): k0 Attack (free: A = fs·v samples, v on [0, 10001) = 0.1 ms steps; sync: q·T_A[rint(127 v)]),
// k1 Attack curve, k2 Release (free: 8·fs·v; sync: q·T_R[rint(127 v)]), k3 Release curve, k4 unused (FX-062),
// k5 Depth. Options (codes = positions in kDuckerOptions): 0 SYNC, 1 Source (type1: track 0…15, FX-1262: a new Ducker
// follows its own track — core::SelectFxType sets it); own code kOptFollowMidi = 2 (FX-1263, flag fx.ducker.followMidi,
// not in the descriptor: the model has no field for it yet).
// Sync tables: Sixteen's own by formula (FX-1210, engine/fx/dsp/ducker_tables.h).
//
// Quirks (FX-052/053, per-effect bits of the quirk mask): kQuirkEdgeTrigger = FX-1280 (TRIGGER acts on an off → on
// edge, so a trigger in the block after a block that fired is missed; Sixteen: every block's trigger fires),
// kQuirkNoClamp = FX-1225 (Sixteen clamps g at 0). FX-1281 is not reproduced, FX-1282 unreachable (Source clamped).
// The reference constructor leaves the attack/release lengths at 44 samples / step 1.0 until ATTACK, RELEASE,
// SYNC or BPM changes; a first parameter set equal to those constructor values (e.g. ATTACK 0.5 = 500 ms) would keep
// them. Sixteen marks the lengths dirty when the instance is prepared or cleared, so they always follow the parameters.
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"
#include "engine/fx/dsp/Numerics.h"

#include <cstdint>
#include <string_view>

namespace engine::fx::effects
{
inline constexpr std::string_view kDuckerOptions[] = {"fx.ducker.sync", "fx.ducker.type1"};
inline constexpr std::string_view kDuckerFollowMidiFlag = "fx.ducker.followMidi"; // FX-1263

class Ducker final : public EffectBase
{
public:
    enum Option : int
    {
        kOptSync = 0,      // "fx.ducker.sync"
        kOptSource = 1,    // "fx.ducker.type1": track 0…15
        kOptFollowMidi = 2 // FX-1263 (P3, off by default)
    };
    enum Telemetry : int
    {
        kTelEnvelope = 0, // the gain reduction now, 1 − g (0 … Depth)
        kTelGain = 1      // g
    };
    static constexpr std::uint32_t kQuirkEdgeTrigger = 1u << 0; // FX-1280
    static constexpr std::uint32_t kQuirkNoClamp = 1u << 1;     // FX-1225
    static constexpr int kStep = 11;                            // control-point spacing (samples)

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-1261: re-applies the parameters only; a duck in progress continues
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override;
    void process(float* left, float* right, const ProcessContext&) noexcept override;
    void setQuirkProfile(std::uint32_t mask) noexcept override
    {
        edgeTrigger_ = (mask & kQuirkEdgeTrigger) != 0;
        clampGain_ = (mask & kQuirkNoClamp) == 0;
    }
    float telemetry(int which) const noexcept override;

    // The block with an explicit trigger (the model's set_param(7, bpm), (8, flag), (9, offset), then process):
    // the golden subject and the tests drive it directly. Neon selects the vectorised multiply.
    void processScalar(float* left, float* right, float bpm, bool trigger, int offset) noexcept;
#if APP_FX_HAVE_NEON
    void processNeon(float* left, float* right, float bpm, bool trigger, int offset) noexcept;
#endif

    // Test access.
    int state() const noexcept { return state_; } // 0 idle, 1 attack, 2 release
    float gain() const noexcept { return g_; }
    int attackSamples() const noexcept { return na_; }
    float releaseStep() const noexcept { return pdec_; }
    int source() const noexcept { return source_; }

private:
    struct Lin // LinearParameter: last / current, change within ±FLT_MIN is no change
    {
        float last, cur;
        bool changed() noexcept;
    };

    template <bool Neon> void run(float* left, float* right, float bpm, bool trigger, int offset) noexcept;
    void update() noexcept;
    int attackSeg(int pos, int n) noexcept;  // returns the new pos (n consumed through n_)
    int releaseSeg(int pos, int n) noexcept;
    void retriggerPhase() noexcept;
    void clear() noexcept;
    bool computeTrace() noexcept; // fills gbuf_; false when the whole block passes unchanged

    Lin p_[6]{{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    Lin bpmP_{120.0f, 120.0f};
    int syncLast_ = 0, syncCur_ = 0;
    int trigLast_ = 0, trigCur_ = 0;
    int offsetCur_ = 0;
    int source_ = 0;
    bool followMidi_ = false, edgeTrigger_ = false, clampGain_ = true;

    // ducker.py fields (+0x544 … +0x594)
    float attack_ = 0.5f, release_ = 0.5f, ca_ = 1.0f, cr_ = 1.0f, depth_ = 0.0f, bpm_ = 120.0f;
    int sync_ = 0, trig_ = 0, offset_ = 0;
    bool dirty_ = true;
    float g_ = 1.0f, incA_ = 0.0f, incR_ = 0.0f;
    int cnt_ = 0;
    bool aDone_ = false, rDone_ = false;
    float x_ = 0.0f, pinc_ = 1.0f, pdec_ = 1.0f;
    int na_ = 44, i_ = 0, state_ = 0;
    int n_ = 0; // samples left in the block (segment bookkeeping)

    double fs_ = 44100.0;
    alignas(16) float gbuf_[kBlockFrames]{};
};
} // namespace engine::fx::effects
