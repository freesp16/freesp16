#include "engine/voice/stretch/StretchFft.h"

#include "engine/voice/stretch/StretchKernels.h"

#include <cmath>
#include <utility>

namespace engine::voice::stretch
{
void StretchFft::prepare(int size)
{
    int n = 4;
    while (n < size)
        n <<= 1;
    n_ = n;
    int bits = 0;
    while ((1 << bits) < n_)
        ++bits;
    swaps_.clear();
    for (int i = 0; i < n_; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        if (i < r)
        {
            swaps_.push_back(i); // rt-ok: prepare(), message thread
            swaps_.push_back(r); // rt-ok: prepare(), message thread
        }
    }
    wr_.assign(static_cast<std::size_t>(n_), 0.0f);
    wi_.assign(static_cast<std::size_t>(n_), 0.0f);
    for (int h = 1; h < n_; h <<= 1)
        for (int j = 0; j < h; ++j)
        {
            const double a = -kernels::kPiD * j / h; // e^(−iπj/h): the stage's twiddles (forward transform)
            wr_[static_cast<std::size_t>(h - 1 + j)] = static_cast<float>(std::cos(a));
            wi_[static_cast<std::size_t>(h - 1 + j)] = static_cast<float>(std::sin(a));
        }
}

void StretchFft::permute(float* re, float* im) const noexcept
{
    const int* s = swaps_.data();
    const auto count = swaps_.size();
    for (std::size_t k = 0; k < count; k += 2)
    {
        std::swap(re[s[k]], re[s[k + 1]]);
        std::swap(im[s[k]], im[s[k + 1]]);
    }
}

void StretchFft::forward(float* re, float* im) const noexcept
{
    permute(re, im);
    // h = 1: twiddle 1.
    for (int b = 0; b < n_; b += 2)
    {
        const float ar = re[b], ai = im[b], br = re[b + 1], bi = im[b + 1];
        re[b] = ar + br;
        im[b] = ai + bi;
        re[b + 1] = ar - br;
        im[b + 1] = ai - bi;
    }
    // h = 2: twiddles 1, −i.
    for (int b = 0; b < n_; b += 4)
    {
        float ar = re[b], ai = im[b], br = re[b + 2], bi = im[b + 2];
        re[b] = ar + br;
        im[b] = ai + bi;
        re[b + 2] = ar - br;
        im[b + 2] = ai - bi;
        ar = re[b + 1];
        ai = im[b + 1];
        br = im[b + 3];  // (br + i·bi)·(−i) = bi − i·br
        bi = -re[b + 3];
        re[b + 1] = ar + br;
        im[b + 1] = ai + bi;
        re[b + 3] = ar - br;
        im[b + 3] = ai - bi;
    }
    for (int h = 4; h < n_; h <<= 1)
        kernels::butterflies(re, im, wr_.data() + (h - 1), wi_.data() + (h - 1), h, n_);
}

void StretchFft::inverse(float* re, float* im) const noexcept
{
    // IDFT·N = conj(DFT(conj(x))).
    for (int i = 0; i < n_; ++i)
        im[i] = -im[i];
    forward(re, im);
    for (int i = 0; i < n_; ++i)
        im[i] = -im[i];
}
} // namespace engine::voice::stretch
