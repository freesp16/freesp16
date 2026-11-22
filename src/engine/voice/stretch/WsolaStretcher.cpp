#include "engine/voice/stretch/WsolaStretcher.h"

#include "engine/voice/stretch/StretchKernels.h"

#include <algorithm>
#include <cmath>

namespace engine::voice::stretch
{
namespace k = kernels;

std::unique_ptr<TimeStretcher> WsolaStretcher::make()
{
    return std::make_unique<WsolaStretcher>(); // rt-ok: factory, message thread (StretcherBank::build)
}

GrainStretcher::Geometry WsolaStretcher::geometryFor(int tier, double sampleRate) const noexcept
{
    const int w = sampleRate > 64000.0 ? 2048 : 1024;
    return {w, w / 2, tier <= 0 ? w / 4 : w / 8, 0};
}

void WsolaStretcher::allocateCore(int)
{
    const Geometry g = geometryFor(0, sampleRate()); // tier 0 has the widest search
    const int w = g.window, overlap = g.window - g.hop, span = 2 * g.search + overlap;
    win_.assign(static_cast<std::size_t>(w), 0.0f);
    for (int i = 0; i < w; ++i) // periodic Hann: sums to 1 at hop W/2
        win_[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * k::kPiD * i / w));
    frame_.assign(static_cast<std::size_t>(w), 0.0f);
    tmp_.assign(static_cast<std::size_t>(std::max(w, span)), 0.0f);
    region_.assign(static_cast<std::size_t>(span), 0.0f);
    tmpl_.assign(static_cast<std::size_t>(overlap), 0.0f);
    regionD_.assign(static_cast<std::size_t>(span / 2), 0.0f);
    tmplD_.assign(static_cast<std::size_t>(overlap / 2), 0.0f);
    energy_.assign(static_cast<std::size_t>(span + 1), 0.0);
    energyD_.assign(static_cast<std::size_t>(span / 2 + 1), 0.0);
}

void WsolaStretcher::resetCore() noexcept { prevActual_ = 0; }

void WsolaStretcher::readMid(std::int64_t start, int n, float* mid) noexcept
{
    readCore(0, start, n, mid);
    if (channels() == 2)
    {
        readCore(1, start, n, tmp_.data());
        for (int i = 0; i < n; ++i)
            mid[i] += tmp_[static_cast<std::size_t>(i)];
    }
}

void WsolaStretcher::synthesise(std::int64_t anaStart, int anaHop) noexcept
{
    const Geometry& g = geometry();
    const int w = g.window, hs = g.hop, overlap = w - hs, delta = g.search;
    std::int64_t actual = anaStart;
    if (anaHop > 0 && delta > 0)
    {
        // The template: the natural continuation of the previous grain over the overlap.
        float* t = tmpl_.data();
        readMid(prevActual_ + hs, overlap, t);
        const float tmplEnergy = k::dot(t, t, overlap);
        if (tmplEnergy > 1.0e-12f)
        {
            const int span = 2 * delta + overlap;
            float* r = region_.data();
            readMid(anaStart - delta, span, r);
            // Coarse pass on the 2× decimated mid signal.
            float* rd = regionD_.data();
            float* td = tmplD_.data();
            const int halfSpan = span / 2, halfOverlap = overlap / 2;
            for (int i = 0; i < halfSpan; ++i)
                rd[i] = r[2 * i] + r[2 * i + 1];
            for (int i = 0; i < halfOverlap; ++i)
                td[i] = t[2 * i] + t[2 * i + 1];
            double* ed = energyD_.data();
            ed[0] = 0.0;
            for (int i = 0; i < halfSpan; ++i)
                ed[i + 1] = ed[i] + static_cast<double>(rd[i]) * static_cast<double>(rd[i]);
            const double eps = 1.0e-9 * overlap;
            const int step = coarseStep();
            auto coarseScore = [&](int d)
            {
                const int off = (d + delta) / 2;
                const double c = k::dot(rd + off, td, halfOverlap);
                return c * std::fabs(c) / (ed[off + halfOverlap] - ed[off] + eps);
            };
            int best = 0;
            double bestScore = coarseScore(0);
            for (int d = -delta; d <= delta; d += step)
            {
                const double s = coarseScore(d);
                if (s > bestScore)
                {
                    bestScore = s;
                    best = d;
                }
            }
            // Fine pass at full resolution around the coarse winner.
            double* e = energy_.data();
            e[0] = 0.0;
            for (int i = 0; i < span; ++i)
                e[i + 1] = e[i] + static_cast<double>(r[i]) * static_cast<double>(r[i]);
            const int lo = std::max(-delta, best - step + 1), hi = std::min(delta, best + step - 1);
            int fine = best;
            double fineScore = -1.0e300;
            for (int d = lo; d <= hi; ++d)
            {
                const int off = d + delta;
                const double c = k::dot(r + off, t, overlap);
                const double s = c * std::fabs(c) / (e[off + overlap] - e[off] + eps);
                if (s > fineScore)
                {
                    fineScore = s;
                    fine = d;
                }
            }
            actual = anaStart + fine;
        }
    }
    prevActual_ = actual;
    for (int c = 0; c < channels(); ++c)
    {
        readCore(c, actual, w, frame_.data());
        k::addWindowed(acc(c), frame_.data(), win_.data(), w);
    }
}

namespace
{
std::unique_ptr<TimeStretcher> makeWsola() { return WsolaStretcher::make(); }
} // namespace
} // namespace engine::voice::stretch

APP_REGISTER_STRETCHER("wsola", &engine::voice::stretch::makeWsola)
