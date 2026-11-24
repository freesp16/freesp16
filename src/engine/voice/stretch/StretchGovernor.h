// The MASTER TEMPO CPU governor. The FX-051 worst case (32 inserts on Distortion — or on the Reverb, which costs about
// the same — ≈ 448k + Reverb, Send Delay and a Master Compressor ≈ 35k) leaves ≈ 170k cycles of the 700k block for
// voices, mixer and sequencer, so all 16 MASTER TEMPO voices cannot run next to it. A track Reverb counts as an
// active insert. Once per block:
//
//   score   = 8 · reverb + active inserts + 2 · MASTER TEMPO players     (the reference heuristic, FX-864)
//   tier    = 1 ("cheaper") when score ≥ 33, else 0
//   predict = base + inserts · insertCycles + reverb · reverbCycles + players · voiceCycles[tier]
//   hold    = predict + voiceCycles[tier] > deadline   → new MASTER TEMPO notes play as RESAMPLE (caution chip)
//
// Sounding stretched notes are never cut (the hold applies to new notes only), so with every voice already stretching
// the prediction may exceed the deadline — the tier switch (applied at each next note) is what brings it back. The
// cycle constants are A9 estimates from the budget table and the bench (tests/bench/bench_stretch.cpp), to be
// replaced with device-bench numbers.
#pragma once

#include <cstdint>

namespace engine::fx
{
class FxSlotBank;
}

namespace engine::voice::stretch
{
struct GovernorInput
{
    int reverb = 0;            // Send FX slot running the Reverb (0/1)
    int activeInserts = 0;     // non-idle insert slots (32 track inserts, plus a non-Reverb Send FX and the Master FX)
    int masterTempoVoices = 0; // voices sounding with a stretcher this block
};

struct GovernorConfig
{
    int lowQualityScore = 33;                 // FX-864
    double deadlineCycles = 700000.0;         // the engine's whole 64-frame block at 44.1 kHz
    double baseCycles = 120000.0;             // 16 plain voices, mixer, sequencer, meters
    double insertCycles = 14000.0;            // the dearest insert (Distortion, FX-051)
    double reverbCycles = 14000.0;            // the FDN Reverb ≈ the dearest insert (tests/bench/fx/reverb.cpp)
    double voiceCycles[2] = {16000.0, 12000.0}; // per MASTER TEMPO stereo voice: tier 0, tier 1 (target)
};

struct GovernorDecision
{
    int score = 0;
    int tier = 0;
    bool holdNew = false;     // new MASTER TEMPO notes play as RESAMPLE
    double predictedCycles = 0.0;
};

GovernorDecision govern(const GovernorInput&, const GovernorConfig& = {}) noexcept;

// The FX part of the input from the slot bank (audio thread, block start; masterTempoVoices left at 0).
GovernorInput governorInput(const fx::FxSlotBank&) noexcept;
} // namespace engine::voice::stretch
