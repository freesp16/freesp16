#include "engine/modulation/Lfo.h"

#include "core/util/lfo/LfoLaw.h"
#include "engine/EngineApi.h"
#include "engine/modulation/Smoothers.h"

#include <cmath>

namespace engine::modulation
{
namespace
{
constexpr double kOneShotHold = 1.0 - 1.0 / 1073741824.0; // the cycle's last instant (2^−30 before its end)
constexpr std::int32_t kMaxRandomSteps = 64;             // a jump of more cycles (locate) steps once
constexpr double kMaxCycles = 1.0e9; // int32 conversions (one VFP instruction on the A9; int64 is a library call)

// Cycle counts stay far below 2^31 in practice (40 Hz runs 1.7 years before 2^31; 24 h at 300 BPM of 1/32T is 5.2 M):
// larger values are folded, which only shifts the Random sequence's cycle index.
double fold(double x) noexcept
{
    return x < kMaxCycles && x > -kMaxCycles ? x : x - 2.0 * kMaxCycles * static_cast<double>(static_cast<std::int32_t>(x / (2.0 * kMaxCycles)));
}
std::int32_t floorToInt(double x) noexcept
{
    const auto i = static_cast<std::int32_t>(x);
    return static_cast<double>(i) > x ? i - 1 : i;
}
double frac(double x) noexcept
{
    return x - static_cast<double>(floorToInt(x));
}
} // namespace

float lfoShapeValue(LfoShape shape, double phase) noexcept
{
    const double p = frac(fold(phase));
    switch (shape)
    {
        case LfoShape::Sine: return sinTurns(p);
        case LfoShape::Triangle:
            return static_cast<float>(p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0));
        case LfoShape::Saw: return static_cast<float>(1.0 - 2.0 * p);
        case LfoShape::Pulse50: return p < 0.5 ? 1.0f : -1.0f;
        case LfoShape::Pulse25: return p < 0.25 ? 1.0f : -1.0f;
        case LfoShape::Pulse12: return p < 0.125 ? 1.0f : -1.0f;
        case LfoShape::Random: break;
    }
    return 0.0f;
}

void Lfo::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    // LFO-011: one pole at 50 Hz, stepped once per 64-frame block (message thread: libm is fine here).
    smoothK_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * 50.0 * kBlockFrames / sampleRate_));
    blockSeconds_ = kBlockFrames / sampleRate_;
    freeRate_ = static_cast<double>(hz_) * blockSeconds_;
    elapsed_ = elapsedNow_ = 0.0;
    anchored_ = false;
    restartPending_ = true; // the first block starts at the phase offset with a fresh Random sequence
    restartExact_ = false;
    smoothY_ = value_ = raw_ = 0.0f;
}

void Lfo::setShape(LfoShape s) noexcept
{
    if (static_cast<int>(s) >= kLfoShapes)
        s = LfoShape::Sine;
    shape_ = s;
}

void Lfo::setSpeedHz(float hz) noexcept
{
    hz_ = ! (hz >= core::lfo::kMinHz) ? core::lfo::kMinHz : (hz > core::lfo::kMaxHz ? core::lfo::kMaxHz : hz);
    freeRate_ = static_cast<double>(hz_) * blockSeconds_;
}

void Lfo::setSyncDivision(int d) noexcept
{
    division_ = d < 0 ? -1 : (d >= core::lfo::kNumSyncDivisions ? core::lfo::kNumSyncDivisions - 1 : d);
    if (division_ >= 0)
    {
        q_ = core::lfo::kSyncDivisions[static_cast<std::size_t>(division_)].quarters;
        invQ_ = 1.0 / q_;
    }
}

void Lfo::setDepth(float d) noexcept { depth_ = ! (d >= 0.0f) ? 0.0f : (d > 1.0f ? 1.0f : d); }

void Lfo::setPhaseOffset(float p) noexcept { offset_ = ! (p >= 0.0f) ? 0.0f : (p >= 1.0f ? 0.0f : p); }

void Lfo::setSeed(std::uint32_t s) noexcept { seed_ = s != 0 ? s : 0x6D2B79F5u; }

void Lfo::setOneShot(bool on) noexcept { oneShot_ = on; }

void Lfo::restart() noexcept
{
    restartPending_ = true;
    restartExact_ = false;
}

void Lfo::restartAt(double ppq) noexcept
{
    restartPending_ = true;
    restartExact_ = true;
    restartPpq_ = ppq;
}

float Lfo::advanceBlock(double bpm, double ppq, bool running) noexcept
{
    const bool sync = division_ >= 0;
    const double q = q_;
    const bool locked = sync && running;
    const bool restarted = restartPending_;
    if (restarted)
    {
        restartPending_ = false;
        elapsed_ = 0.0;
        if (locked)
        {
            anchor_ = restartExact_ ? restartPpq_ : ppq;
            quarters_ = q;
            anchored_ = true;
        }
    }

    double e = elapsed_;
    if (locked)
    {
        if (! anchored_) // into SYNC + running: the phase continues, then follows the PPQ
            anchor_ = ppq - elapsed_ * q;
        else if (q != quarters_) // another division: continuous at this block start, locked to the PPQ after
            anchor_ = ppq - (ppq - anchor_) / quarters_ * q;
        quarters_ = q;
        anchored_ = true;
        e = (ppq - anchor_) * invQ_;
        if (e < 0.0)
            e = 0.0; // a trigger later in this block: the restart shows from the next block
    }
    else
        anchored_ = false;
    if (oneShot_ && e > kOneShotHold)
        e = kOneShotHold;
    elapsedNow_ = e;

    // The next block start for the accumulating modes (in SYNC + running it is recomputed from the PPQ).
    const double rate = sync ? (bpm > 0.0 ? bpm : 0.0) * (1.0 / 60.0) * blockSeconds_ * invQ_ : freeRate_;
    elapsed_ = fold(e + rate);
    if (oneShot_ && elapsed_ > kOneShotHold)
        elapsed_ = kOneShotHold;

    const double total = fold(static_cast<double>(offset_) + e);
    const std::int32_t cycle = floorToInt(total);
    phase_ = total - static_cast<double>(cycle);

    // Random: restarts from the seed, one xorshift32 step per new cycle (LFO-008).
    if (restarted)
    {
        state_ = core::lfo::xorshift32(seed_);
        held_ = core::lfo::bipolar(state_);
        cycle_ = cycle;
    }
    else if (cycle != cycle_)
    {
        std::int32_t steps = cycle - cycle_;
        if (steps < 1 || steps > kMaxRandomSteps)
            steps = 1;
        for (; steps > 0; --steps)
            state_ = core::lfo::xorshift32(state_);
        held_ = core::lfo::bipolar(state_);
        cycle_ = cycle;
    }

    raw_ = shape_ == LfoShape::Random ? held_ : lfoShapeValue(shape_, phase_);
    if (lfoShapeSmoothed(shape_))
        smoothY_ += smoothK_ * (raw_ - smoothY_); // LFO-011
    else
        smoothY_ = raw_;
    value_ = smoothY_;
    return value_;
}
} // namespace engine::modulation
