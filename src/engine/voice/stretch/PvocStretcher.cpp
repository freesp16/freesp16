#include "engine/voice/stretch/PvocStretcher.h"

#include "engine/voice/stretch/StretchKernels.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::voice::stretch
{
namespace k = kernels;

std::unique_ptr<TimeStretcher> PvocStretcher::make()
{
    return std::make_unique<PvocStretcher>(); // rt-ok: factory, message thread (StretcherBank::build)
}

GrainStretcher::Geometry PvocStretcher::geometryFor(int tier, double sampleRate) const noexcept
{
    const int n = sampleRate > 64000.0 ? 2048 : 1024;
    if (tier <= 0)
        return {n, n / 4, 0, n};
    const int hop = (n / 3) & ~3; // 340 at N = 1024: overlap 3
    return {3 * hop, hop, 0, n};
}

void PvocStretcher::allocateCore(int channels)
{
    const Geometry g = geometryFor(0, sampleRate());
    n_ = g.fft;
    bins_ = n_ / 2 + 1;
    fft_.prepare(n_);
    const auto n = static_cast<std::size_t>(n_), b = static_cast<std::size_t>(bins_);
    re_.assign(n, 0.0f);
    im_.assign(n, 0.0f);
    for (int c = 0; c < 2; ++c)
    {
        const bool used = c < channels;
        frame_[c].assign(used ? n : 0u, 0.0f);
        xr_[c].assign(used ? b : 0u, 0.0f);
        xi_[c].assign(used ? b : 0u, 0.0f);
    }
    for (auto* v : {&mr_, &mi_, &mag_, &phase_, &prevPhase_, &prevTheta_, &theta_, &rotC_, &rotS_})
        v->assign(b, 0.0f);
    peaks_.assign(b, 0);
    // Both tiers' windows (no libm on the audio thread): [tier][W].
    anaWin_.assign(2 * n, 0.0f);
    synWin_.assign(2 * n, 0.0f);
    identWin_.assign(2 * n, 0.0f);
    for (int t = 0; t < kTiers; ++t)
    {
        const Geometry gt = geometryFor(t, sampleRate());
        // Σ over the overlapping grains of analysis × synthesis Hann = (W / hop) · 3/8; the inverse FFT is unscaled.
        const double norm = 1.0 / (static_cast<double>(n_) * (static_cast<double>(gt.window) / gt.hop) * 0.375);
        for (int i = 0; i < gt.window; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * k::kPiD * i / gt.window);
            anaWin_[static_cast<std::size_t>(t) * n + static_cast<std::size_t>(i)] = static_cast<float>(w);
            synWin_[static_cast<std::size_t>(t) * n + static_cast<std::size_t>(i)] = static_cast<float>(w * norm);
            identWin_[static_cast<std::size_t>(t) * n + static_cast<std::size_t>(i)] =
                static_cast<float>(w * w * norm * n_); // analysis × synthesis × N: the θ = 0 grain in the time domain
        }
    }
}

void PvocStretcher::resetCore() noexcept
{
    std::fill(prevPhase_.begin(), prevPhase_.end(), 0.0f);
    std::fill(prevTheta_.begin(), prevTheta_.end(), 0.0f);
}

void PvocStretcher::synthesise(std::int64_t anaStart, int anaHop) noexcept
{
    const Geometry& g = geometry();
    const int w = g.window, hs = g.hop, n = n_, half = n_ / 2, b = bins_;
    const bool stereo = channels() == 2;
    const float* aw = anaWin_.data() + static_cast<std::size_t>(quality()) * static_cast<std::size_t>(n);
    const float* sw = synWin_.data() + static_cast<std::size_t>(quality()) * static_cast<std::size_t>(n);
    float* re = re_.data();
    float* im = im_.data();

    // 1  analysis: both channels in one complex FFT (z = L + iR)
    for (int c = 0; c < channels(); ++c)
        readCore(c, anaStart, w, frame_[c].data());
    // Phase reset at the note start: the W/hop grains that overlap output frame 0 keep their analysis phases (θ = 0),
    // so the onset is reproduced instead of smeared by the phase rotation (no pre-echo). Such a grain is its own
    // windowed input — no FFT round trip (this is most of the priming burst); only the last one is analysed, to
    // seed the phase propagation of the grain after it.
    const auto startup = static_cast<std::uint64_t>(w / hs);
    const bool reset = grainIndex() < startup;
    if (reset)
    {
        const float* iw = identWin_.data() + static_cast<std::size_t>(quality()) * static_cast<std::size_t>(n);
        for (int c = 0; c < channels(); ++c)
            k::addWindowed(acc(c), frame_[c].data(), iw, w);
        if (grainIndex() + 1 < startup)
            return;
    }
    k::mulWindow(re, frame_[0].data(), aw, w);
    if (stereo)
        k::mulWindow(im, frame_[1].data(), aw, w);
    else
        std::fill(im, im + w, 0.0f);
    std::fill(re + w, re + n, 0.0f);
    std::fill(im + w, im + n, 0.0f);
    fft_.forward(re, im);

    float* lr = xr_[0].data();
    float* li = xi_[0].data();
    float* rr = stereo ? xr_[1].data() : nullptr;
    float* ri = stereo ? xi_[1].data() : nullptr;
    float* mr = mr_.data();
    float* mi = mi_.data();
    if (stereo)
        for (int i = 0; i < b; ++i)
        {
            const int j = (n - i) & (n - 1);
            const float zr = re[i], zi = im[i], nr = re[j], ni = im[j];
            lr[i] = 0.5f * (zr + nr);
            li[i] = 0.5f * (zi - ni);
            rr[i] = 0.5f * (zi + ni);
            ri[i] = 0.5f * (nr - zr);
            mr[i] = lr[i] + rr[i];
            mi[i] = li[i] + ri[i];
        }
    else
        for (int i = 0; i < b; ++i)
        {
            lr[i] = mr[i] = re[i];
            li[i] = mi[i] = im[i];
        }
    k::atan2(phase_.data(), mi, mr, b);
    if (reset)
    {
        std::memcpy(prevPhase_.data(), phase_.data(), static_cast<std::size_t>(b) * sizeof(float));
        std::fill(prevTheta_.begin(), prevTheta_.end(), 0.0f);
        return;
    }
    k::magSq(mag_.data(), mr, mi, b);

    // 2  phase propagation at the peaks, identity locking elsewhere
    float* theta = theta_.data();
    float* cr = rotC_.data();
    float* ci = rotS_.data();
    const float* mag = mag_.data();
    const float* ph = phase_.data();
    int peaks = 0;
    if (anaHop > 0)
    {
        float top = 0.0f;
        for (int i = 1; i < half; ++i)
            top = mag[i] > top ? mag[i] : top;
        const float floorMag = top * 1.0e-10f; // −100 dB
        for (int i = 1; i < half; ++i)
            if (mag[i] > floorMag && mag[i] > mag[i - 1] && mag[i] >= mag[i + 1])
                peaks_[static_cast<std::size_t>(peaks++)] = i;
    }
    if (peaks == 0)
    {
        std::fill(theta, theta + b, 0.0f);
        std::fill(cr, cr + b, 1.0f);
        std::fill(ci, ci + b, 0.0f);
    }
    else
    {
        const float binToRad = k::kTwoPi / static_cast<float>(n);
        const float hopRatio = static_cast<float>(hs) / static_cast<float>(anaHop);
        int start = 0;
        for (int p = 0; p < peaks; ++p)
        {
            const int pk = peaks_[static_cast<std::size_t>(p)];
            const auto expected = static_cast<float>((static_cast<std::int64_t>(pk) * anaHop) & (n - 1)) * binToRad;
            const float dev = k::princarg(ph[pk] - prevPhase_[static_cast<std::size_t>(pk)] - expected);
            const auto advance = static_cast<float>((static_cast<std::int64_t>(pk) * hs) & (n - 1)) * binToRad;
            const float psi = prevPhase_[static_cast<std::size_t>(pk)] + prevTheta_[static_cast<std::size_t>(pk)] +
                              advance + dev * hopRatio;
            const float th = k::princarg(psi - ph[pk]);
            float s = 0.0f, c = 1.0f;
            k::sinCos(th, s, c);
            int end = b;
            if (p + 1 < peaks) // the region ends at the lowest bin between this peak and the next
            {
                const int next = peaks_[static_cast<std::size_t>(p + 1)];
                end = pk + 1;
                for (int i = pk + 2; i <= next; ++i)
                    end = mag[i] < mag[end] ? i : end;
            }
            for (int i = start; i < end; ++i)
            {
                theta[i] = th;
                cr[i] = c;
                ci[i] = s;
            }
            start = end;
        }
        theta[0] = theta[half] = 0.0f; // DC and Nyquist stay real
        cr[0] = cr[half] = 1.0f;
        ci[0] = ci[half] = 0.0f;
    }
    k::rotate(lr, li, cr, ci, b);
    if (stereo)
        k::rotate(rr, ri, cr, ci, b);
    std::memcpy(prevPhase_.data(), ph, static_cast<std::size_t>(b) * sizeof(float));
    std::memcpy(prevTheta_.data(), theta, static_cast<std::size_t>(b) * sizeof(float));

    // 3  synthesis: Z = Y_L + i·Y_R (Hermitian halves mirrored), inverse FFT, Hann, overlap-add
    if (stereo)
    {
        for (int i = 0; i <= half; ++i)
        {
            re[i] = lr[i] - ri[i];
            im[i] = li[i] + rr[i];
        }
        for (int i = 1; i < half; ++i)
        {
            re[n - i] = lr[i] + ri[i];
            im[n - i] = rr[i] - li[i];
        }
    }
    else
    {
        for (int i = 0; i <= half; ++i)
        {
            re[i] = lr[i];
            im[i] = li[i];
        }
        for (int i = 1; i < half; ++i)
        {
            re[n - i] = lr[i];
            im[n - i] = -li[i];
        }
    }
    fft_.inverse(re, im);
    k::addWindowed(acc(0), re, sw, w);
    if (stereo)
        k::addWindowed(acc(1), im, sw, w);
}

namespace
{
std::unique_ptr<TimeStretcher> makePvoc() { return PvocStretcher::make(); }
} // namespace
} // namespace engine::voice::stretch

APP_REGISTER_STRETCHER("pvoc", &engine::voice::stretch::makePvoc)
