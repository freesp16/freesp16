// An independent double-precision implementation of the AFS digital model (afs_ladder.h, ModelProcessor) for the
// golden comparison: the same half-band taps (widened), the exact prewarped G = tan(π·fc/fs2)/(1 + tan) at every
// internal sample (no table), the same smoothers, saturator, ladder solve and high-pass in double. The C++ model must
// stay within the golden harness's default tolerance of it (1e-5 relative to peak / ≤ −100 dBFS residual where the
// model is smooth; see test_afs_model.cpp for the cases).
#pragma once

#include "engine/afs/AnalogFilterSlot.h"
#include "engine/fx/dsp/afs_ladder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace afstest
{
class ModelReference
{
public:
    explicit ModelReference(double fs) : fs_(fs)
    {
        std::uint32_t x = 0x5EED0AF5u; // the model's −120 dBFS seed, regenerated
        for (float& n : noise_)
        {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            n = (static_cast<float>(x >> 8) * (1.0f / 16777216.0f) - 0.5f) * 2.0e-6f;
        }
        for (int j = 0; j < kTaps; ++j)
            h_[j] = engine::fx::dsp::afs::kHalfbandOdd[j];
        upL_.assign(kTaps - 1, 0.0);
        upR_ = upL_;
        dOddL_.assign(kTaps - 1, 0.0);
        dOddR_ = dOddL_;
        dEvenL_.assign(kHalf - 1, 0.0);
        dEvenR_ = dEvenL_;
    }

    // One 64-frame block with fixed knobs (lpCutoff/resonance 0–1023, hp 0–1023, the drive already applied).
    void process(float* l, float* r, int lp, int res, int hp, bool twoPole)
    {
        constexpr int B = engine::fx::dsp::afs::kBlock;
        const double fs2 = 2.0 * fs_;
        const double knob = lp;
        const double kT = engine::afs::ModelProcessor::feedback(static_cast<float>(res) / 1023.0f);
        const double smooth = 1.0 - std::exp(-1.0 / (0.001 * fs2));
        if (! primed_)
        {
            pos_ = knob;
            k_ = kT;
            primed_ = true;
        }
        // Up.
        std::vector<double> in2(4 * B);
        for (int c = 0; c < 2; ++c)
        {
            auto& hist = c == 0 ? upL_ : upR_;
            const float* x = c == 0 ? l : r;
            for (int n = 0; n < B; ++n)
                hist.push_back(x[n]);
            for (int n = 0; n < B; ++n)
            {
                const std::size_t at = static_cast<std::size_t>(kTaps - 1 + n);
                double acc = 0.0;
                for (int j = 0; j < kTaps; ++j)
                    acc += 2.0 * h_[j] * hist[at - static_cast<std::size_t>(j)];
                in2[static_cast<std::size_t>(4 * n + c)] = hist[at - kHalf];
                in2[static_cast<std::size_t>(4 * n + 2 + c)] = acc;
            }
            hist.erase(hist.begin(), hist.begin() + B);
        }
        // Ladder.
        std::vector<double> out2(4 * B);
        const float* nz = noise_ + static_cast<std::size_t>(noisePos_) * 4 * B;
        noisePos_ = (noisePos_ + 1) & 7;
        for (int i = 0; i < 2 * B; ++i)
        {
            pos_ += (knob - pos_) * smooth;
            k_ += (kT - k_) * smooth;
            const double fc = std::min(20.0 * std::pow(1000.0, pos_ / 1023.0), 0.45 * fs2);
            const double g = std::tan(3.14159265358979323846 * fc / fs2), G = g / (1.0 + g);
            const double inv = 1.0 / (1.0 + k_ * G * G * G * G);
            for (int c = 0; c < 2; ++c)
            {
                double* s = s_[c];
                const double x = in2[static_cast<std::size_t>(2 * i + c)] + static_cast<double>(nz[2 * i + c]);
                const double S = (((1 - G) * s[0] * G + (1 - G) * s[1]) * G + (1 - G) * s[2]) * G + (1 - G) * s[3];
                const double u = sat((x - k_ * S) * inv);
                double v = (u - s[0]) * G;
                const double y1 = v + s[0];
                s[0] = y1 + v;
                v = (sat(y1) - s[1]) * G;
                const double y2 = v + s[1];
                s[1] = y2 + v;
                v = (sat(y2) - s[2]) * G;
                const double y3 = v + s[2];
                s[2] = y3 + v;
                v = (sat(y3) - s[3]) * G;
                const double y4 = v + s[3];
                s[3] = y4 + v;
                out2[static_cast<std::size_t>(2 * i + c)] = twoPole ? y2 : y4;
            }
        }
        // Down.
        for (int c = 0; c < 2; ++c)
        {
            auto& odd = c == 0 ? dOddL_ : dOddR_;
            auto& even = c == 0 ? dEvenL_ : dEvenR_;
            for (int n = 0; n < B; ++n)
            {
                even.push_back(out2[static_cast<std::size_t>(4 * n + c)]);
                odd.push_back(out2[static_cast<std::size_t>(4 * n + 2 + c)]);
            }
            float* y = c == 0 ? l : r;
            for (int n = 0; n < B; ++n)
            {
                double acc = 0.0;
                for (int j = 0; j < kTaps; ++j)
                    acc += h_[j] * odd[static_cast<std::size_t>(kTaps - 1 + n - j)];
                acc += 0.5 * even[static_cast<std::size_t>(n)];
                y[n] = static_cast<float>(hpStep(c, acc, hp));
            }
            odd.erase(odd.begin(), odd.begin() + B);
            even.erase(even.begin(), even.begin() + B);
        }
    }

private:
    static constexpr int kTaps = engine::fx::dsp::afs::kPhaseTaps;
    static constexpr int kHalf = engine::fx::dsp::afs::kHalfK;
    static double sat(double v)
    {
        constexpr double c = 2.25, a = 2.0 / (3.0 * c * c), b = 1.0 / (5.0 * c * c * c * c);
        const double w = std::clamp(v, -c, c), w2 = w * w;
        return w * (1.0 - w2 * (a - b * w2));
    }
    double hpStep(int c, double x, int hp)
    {
        if (hp <= 0)
            return x;
        const double hz = 20.0 * std::pow(100.0, (hp - 1) / 1022.0);
        // TPT SVF high-pass; the coefficients in double rounded to float, as the default maths does.
        const double gd = std::tan(3.14159265358979323846 * hz / fs_), R = 0.70710678118654752;
        const double g = static_cast<float>(gd), r2g = static_cast<float>(2.0 * R + gd),
                     h = static_cast<float>(1.0 / (1.0 + 2.0 * R * gd + gd * gd));
        auto& st = hp_[c];
        const double y = (x - r2g * st[0] - st[1]) * h;
        const double b = g * y + st[0];
        st[0] = g * y + b;
        const double lo = g * b + st[1];
        st[1] = g * b + lo;
        return y;
    }

    double fs_;
    double h_[kTaps] {};
    std::vector<double> upL_, upR_, dOddL_, dOddR_, dEvenL_, dEvenR_;
    double s_[2][4] {};
    double hp_[2][4] {};
    double pos_ = 0, k_ = 0;
    bool primed_ = false;
    float noise_[2 * engine::fx::dsp::afs::kOver * 8] {};
    int noisePos_ = 0;
};
} // namespace afstest
