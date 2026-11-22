// Ducker — see Ducker.h. Line for line the MIT model tools/fx/ducker.py (research pinned commit 8901c97); F(x) there =
// a float32 rounding here (float + − × ÷ of two floats equal the model's double operation rounded once; its double
// steps — the curve pow and the target sum — are written out in double).
#include "engine/fx/effects/Ducker.h"

#include "engine/fx/dsp/Vec.h"
#include "engine/fx/dsp/ducker_tables.h"

#include <cfloat>
#include <cmath>

namespace engine::fx::effects
{
namespace
{
const dsp::ducker::SyncTables kTables{}; // FX-1210, built before main()

constexpr double kEpsD = 1.1920928955078125e-07; // 2^-23 (double literal: the curve |c − 1| test)
constexpr float kEpsF = 1.1920929e-07f;          // 2^-23 (float literal: the retrigger DEPTH test)
constexpr float kQuarter = 0.25f, k60 = 60.0f, k127 = 127.0f, kMilli = 0.001f, k8 = 8.0f;
constexpr float kStepF = static_cast<float>(Ducker::kStep);

int truncS32(float x) noexcept // vcvt.s32.f32 (saturating, NaN → 0)
{
    if (! (x == x))
        return 0;
    if (x >= 2147483647.0f)
        return 2147483647;
    if (x <= -2147483648.0f)
        return -2147483647 - 1;
    return static_cast<int>(x);
}
int rintEven(float x) noexcept
{
    return static_cast<int>(std::nearbyint(static_cast<double>(x)));
}
// The curve base c = F(pow(100, F(F(2v) − 1))) (the model: correctly rounded double pow, rounded to float).
float curveOf(float v) noexcept
{
    const float e = (v + v) - 1.0f;
    return static_cast<float>(std::pow(100.0, static_cast<double>(e)));
}
bool curved(float c) noexcept
{
    const double dc = static_cast<double>(c);
    return (dc - 1.0 > kEpsD) || (1.0 - dc > kEpsD);
}
// Default maths for the model's fdlibm log10f port (retrigger only).
float log10F(float x) noexcept
{
    return static_cast<float>(std::log10(static_cast<double>(x)));
}
} // namespace

bool Ducker::Lin::changed() noexcept
{
    const float d = last - cur;
    if (d <= FLT_MIN && -FLT_MIN <= d)
        return false;
    last = cur;
    return true;
}

void Ducker::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    clear();
}

void Ducker::clear() noexcept // ducker.py constructor state (+ Sixteen's dirty lengths, see Ducker.h)
{
    for (int k = 0; k < 4; ++k)
        p_[k] = {0.5f, 0.5f};
    p_[4] = p_[5] = {0.0f, 0.0f};
    bpmP_ = {120.0f, 120.0f};
    syncLast_ = syncCur_ = trigLast_ = trigCur_ = 0;
    offsetCur_ = 0;
    attack_ = release_ = 0.5f;
    ca_ = cr_ = 1.0f;
    depth_ = 0.0f;
    bpm_ = 120.0f;
    sync_ = trig_ = offset_ = 0;
    dirty_ = true;
    g_ = 1.0f;
    incA_ = incR_ = 0.0f;
    cnt_ = 0;
    aDone_ = rDone_ = false;
    x_ = 0.0f;
    pinc_ = pdec_ = 1.0f;
    na_ = 44;
    i_ = 0;
    state_ = 0;
}

void Ducker::reset() noexcept // slot 27: run the parameter update only (FX-1261)
{
    update();
}

void Ducker::setParam(int index, float value01) noexcept
{
    if (index >= 0 && index < 6)
        p_[index].cur = value01;
}

void Ducker::setOption(int which, int value) noexcept
{
    switch (which)
    {
    case kOptSync:
        syncCur_ = value != 0 ? 1 : 0; // ToggleParameter
        break;
    case kOptSource:
        source_ = value < 0 ? 0 : (value > kTracks - 1 ? kTracks - 1 : value); // FX-1282: clamped
        break;
    case kOptFollowMidi:
        followMidi_ = value != 0;
        break;
    case kClearState:
        if (value != 0)
            clear();
        break;
    default:
        break;
    }
}

float Ducker::telemetry(int which) const noexcept
{
    switch (which)
    {
    case kTelEnvelope:
        return 1.0f - g_;
    case kTelGain:
        return g_;
    default:
        return 0.0f;
    }
}

void Ducker::update() noexcept // 0xf6630
{
    APP_FX_NOFUSE
    // TRIGGER: the reference toggle acts on an off → on edge (FX-1280); Sixteen fires on every block's trigger.
    bool fire = false;
    if (edgeTrigger_)
    {
        if (trigCur_ != trigLast_)
        {
            trigLast_ = trigCur_;
            fire = trigCur_ != 0;
        }
    }
    else
        fire = trigCur_ != 0;
    if (fire)
    {
        if (state_ == 0 || state_ == 2)
        {
            cnt_ = 0;
            offset_ = offsetCur_;
            trig_ = 1;
        }
        i_ = 0;
        trigCur_ = 0; // written back to off (current value only)
    }
    if (! edgeTrigger_)
        trigCur_ = 0; // Sixteen: one block's trigger fires once (a reset() in between does not fire it again)
    if (p_[1].changed())
        ca_ = curveOf(p_[1].cur);
    if (p_[3].changed())
        cr_ = curveOf(p_[3].cur);
    if (p_[5].changed())
        depth_ = p_[5].cur;
    if (bpmP_.changed())
    {
        bpm_ = bpmP_.cur;
        dirty_ = true;
    }
    if (syncCur_ != syncLast_)
    {
        syncLast_ = syncCur_;
        sync_ = syncCur_;
        dirty_ = true;
    }
    if (p_[0].changed())
    {
        attack_ = p_[0].cur;
        dirty_ = true;
    }
    if (p_[2].changed())
    {
        release_ = p_[2].cur;
        dirty_ = true;
    }
    if (! dirty_)
        return;
    const float fs = static_cast<float>(fs_);
    float A, Rl;
    if (sync_ != 0)
    {
        const float q = ((fs * kQuarter) * k60) / bpm_;
        int ia = rintEven(attack_ * k127), ir = rintEven(release_ * k127);
        ia = ia < 0 ? 0 : (ia > 127 ? 127 : ia);
        ir = ir < 0 ? 0 : (ir > 127 ? 127 : ir);
        A = q * kTables.attack[ia];
        Rl = q * kTables.release[ir];
    }
    else
    {
        A = fs * attack_;
        Rl = (fs * release_) * k8;
    }
    if (A < 1.0f)
    {
        na_ = 1;
        pinc_ = 1.0f;
    }
    else
    {
        na_ = truncS32(A);
        pinc_ = 1.0f / static_cast<float>(na_);
    }
    dirty_ = false;
    float m = fs * kMilli;
    if (m <= Rl)
        m = Rl;
    pdec_ = 1.0f / m;
}

int Ducker::attackSeg(int pos, int n) noexcept // 0xf69e4
{
    APP_FX_NOFUSE
    const float c = ca_, D = depth_;
    const int Na = na_;
    float g = g_, x = x_, inc = incA_;
    int cnt = cnt_, i = i_;
    const double dc = static_cast<double>(c);
    if (n <= 0)
    {
    }
    else if (Na <= i)
    {
        aDone_ = true;
        cnt = 0;
        x = 1.0f;
    }
    else if (curved(c))
    {
        int r5 = Na - i;
        const double cm1 = static_cast<double>(c - 1.0f);
        const double oneMinusD = static_cast<double>(1.0f - D), dd = static_cast<double>(D);
        for (;;)
        {
            if (--cnt <= 0)
            {
                x = x + pinc_ * kStepF;
                cnt = r5 < kStep ? r5 : kStep;
                if (x > 1.0f)
                    x = 1.0f;
                const double pw = std::pow(dc, static_cast<double>(x));
                const double d18 = 1.0 - (pw - 1.0) / cm1;
                const float tgt = static_cast<float>(oneMinusD + d18 * dd);
                inc = (tgt - g) / static_cast<float>(cnt);
            }
            g = g + inc;
            if (clampGain_ && g < 0.0f)
                g = 0.0f; // FX-1225
            gbuf_[pos++] = g;
            --n;
            i = Na + 1 - r5;
            if (n <= 0)
                break;
            if (--r5 == 0)
            {
                aDone_ = true;
                cnt = 0;
                x = 1.0f;
                break;
            }
        }
    }
    else
    {
        int r0 = Na - i;
        for (;;)
        {
            if (--cnt <= 0)
            {
                x = x + pinc_ * kStepF;
                cnt = r0 < kStep ? r0 : kStep;
                float s14 = 0.0f, s15 = 1.0f - D;
                if (x <= 1.0f)
                    s14 = 1.0f - x;
                s15 = (s15 + D * s14) - g;
                if (x > 1.0f)
                    x = 1.0f;
                inc = s15 / static_cast<float>(cnt);
            }
            g = g + inc;
            if (clampGain_ && g < 0.0f)
                g = 0.0f;
            gbuf_[pos++] = g;
            --n;
            ++i;
            --r0;
            if (n == 0)
                break;
            if (i == Na)
            {
                aDone_ = true;
                cnt = 0;
                x = 1.0f;
                break;
            }
        }
    }
    g_ = g;
    x_ = x;
    i_ = i;
    incA_ = inc;
    cnt_ = cnt;
    n_ = n;
    return pos;
}

int Ducker::releaseSeg(int pos, int n) noexcept // 0xf6c6c
{
    APP_FX_NOFUSE
    const float c = cr_, D = depth_, r = pdec_;
    float g = g_, x = x_, inc = incR_;
    int cnt = cnt_;
    const bool curve = curved(c);
    const double dc = static_cast<double>(c);
    bool done = false;
    if (n > 0)
    {
        x = x - r;
        if (! (x > 0.0f))
            done = true;
        while (! done)
        {
            if (--cnt <= 0)
            {
                const float s14 = x / r;
                float y = (1.0f - x) + r * kStepF;
                cnt = truncS32(s14);
                if (cnt < 1)
                    cnt = 1;
                if (cnt >= kStep)
                    cnt = kStep;
                if (y > 1.0f)
                    y = 1.0f;
                if (curve)
                {
                    const double pw = std::pow(dc, static_cast<double>(y));
                    const double d18 = (pw - 1.0) / static_cast<double>(c - 1.0f);
                    const float tgt =
                        static_cast<float>(static_cast<double>(1.0f - D) + d18 * static_cast<double>(D));
                    inc = (tgt - g) / static_cast<float>(cnt);
                }
                else
                    inc = (((1.0f - D) + y * D) - g) / static_cast<float>(cnt);
            }
            g = g + inc;
            if (clampGain_ && g < 0.0f)
                g = 0.0f;
            gbuf_[pos++] = g;
            --n;
            if (n <= 0)
                break;
            x = x - r;
            if (! (x > 0.0f))
                done = true;
        }
    }
    if (done)
    {
        rDone_ = true;
        x = 0.0f;
        g = 1.0f;
    }
    g_ = g;
    x_ = x;
    incR_ = inc;
    cnt_ = cnt;
    n_ = n;
    return pos;
}

void Ducker::retriggerPhase() noexcept // the release → attack re-entry (FX-1224)
{
    APP_FX_NOFUSE
    const float D = depth_;
    trig_ = 0;
    if (! (D > kEpsF))
        return;
    const float c = ca_;
    if (curved(c))
    {
        const float s17 = (g_ + (D + c * (1.0f - g_))) - 1.0f;
        if (s17 <= 0.0f)
            x_ = 0.0f;
        else
        {
            const float lc = log10F(c);
            x_ = log10F(s17 / D) / lc;
        }
    }
    else
        x_ = (1.0f - g_) / D;
}

bool Ducker::computeTrace() noexcept // slot 25 0xf6ef4 after the update
{
    int n = kBlockFrames, pos = 0;
    bool touched = false;
    while (n > 0)
    {
        if (state_ == 1)
        {
            if (! touched)
            {
                for (int i = 0; i < kBlockFrames; ++i)
                    gbuf_[i] = 1.0f;
                touched = true;
            }
            pos = attackSeg(pos, n);
            n = n_;
            if (aDone_)
            {
                aDone_ = false;
                state_ = 2;
            }
        }
        else if (state_ == 2)
        {
            if (trig_ == 0)
            {
                if (! touched)
                {
                    for (int i = 0; i < kBlockFrames; ++i)
                        gbuf_[i] = 1.0f;
                    touched = true;
                }
                pos = releaseSeg(pos, n);
                n = n_;
                if (rDone_)
                {
                    state_ = 0;
                    rDone_ = false;
                }
            }
            else
            {
                retriggerPhase();
                state_ = 1;
            }
        }
        else
        {
            if (trig_ == 0)
                break;
            const int off = offset_;
            if (off > 0)
            {
                if (off > n)
                    break;
                pos += off;
                n -= off;
                offset_ = 0;
            }
            trig_ = 0;
            state_ = 1;
        }
    }
    return touched;
}

template <bool Neon> void Ducker::run(float* left, float* right, float bpm, bool trigger, int offset) noexcept
{
    bpmP_.cur = bpm;           // set_param(7, bpm)
    trigCur_ = trigger ? 1 : 0; // set_param(8, flag)
    offsetCur_ = offset;       // set_param(9, offset)
    update();
    if (state_ == 0 && trig_ == 0)
        return; // FX-1260: an idle Ducker passes its input unchanged
    if (! computeTrace())
        return;
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        dsp::vec::neon::mulv(left, gbuf_, kBlockFrames);
        dsp::vec::neon::mulv(right, gbuf_, kBlockFrames);
        return;
    }
#endif
    dsp::vec::scalar::mulv(left, gbuf_, kBlockFrames);
    dsp::vec::scalar::mulv(right, gbuf_, kBlockFrames);
}

void Ducker::processScalar(float* left, float* right, float bpm, bool trigger, int offset) noexcept
{
    run<false>(left, right, bpm, trigger, offset);
}

#if APP_FX_HAVE_NEON
void Ducker::processNeon(float* left, float* right, float bpm, bool trigger, int offset) noexcept
{
    run<true>(left, right, bpm, trigger, offset);
}
#endif

void Ducker::process(float* left, float* right, const ProcessContext& ctx) noexcept
{
    // The host rule (F201): this block's trigger slot of the Source track (FX-1242/1243); MIDI sources never duck
    // unless FX-1263 is on (FX-1241). BPM as fed by the processor (clamped, FX-049).
    const int s = source_;
    bool trigger = ((ctx.triggerMask >> s) & 1u) != 0;
    if (trigger && ((ctx.midiTrackMask >> s) & 1u) != 0 && ! followMidi_)
        trigger = false;
    int offset = trigger ? ctx.triggerOffset[static_cast<std::size_t>(s)] : 0;
    offset = offset < 0 ? 0 : (offset > kBlockFrames - 1 ? kBlockFrames - 1 : offset);
    const float bpm = static_cast<float>(ctx.bpm);
#if APP_FX_HAVE_NEON
    run<true>(left, right, bpm, trigger, offset);
#else
    run<false>(left, right, bpm, trigger, offset);
#endif
}
} // namespace engine::fx::effects

// Registration: Insert only (FX-1244), shell gate 0, no wet high-pass, dry 0 / wet 1 (FX-1260).
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "ducker",
    .name = "DUCKER",
    .slots = static_cast<std::uint8_t>(engine::fx::SlotKind::Insert),
    .params = {"fx.ducker.attack", "fx.ducker.attackCurve", "fx.ducker.release", "fx.ducker.releaseCurve", "",
               "fx.ducker.depth"},
    .options = engine::fx::effects::kDuckerOptions,
    .visual = "duckEnvelope",
    .shell = {},
    .create = &engine::fx::make<engine::fx::effects::Ducker>}))
