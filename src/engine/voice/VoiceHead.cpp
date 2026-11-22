#include "engine/voice/VoiceHead.h"

#include <cstring>

namespace engine::voice
{
namespace
{
using kernels::kOne;

int channelsOf(const Head& h) noexcept { return h.buf->channels >= 2 ? 2 : 1; }

// Frames until d reaches B (d < B), at most cap; 0 when d >= B.
int framesUntil(Fixed d, Fixed boundary, Fixed step, int cap) noexcept
{
    if (d >= boundary)
        return 0;
    const Fixed q = (boundary - d + step - 1) / step;
    return q > cap ? cap : static_cast<int>(q);
}

void setGeometry(Head& h, const Region& r) noexcept
{
    const std::int64_t frames = h.buf->frames, end = r.end();
    const std::int64_t length = end - r.loopStart;
    h.loop = r.loop;
    h.loopLen = length * kOne;
    std::int64_t preroll;
    if (h.dir > 0)
    {
        h.stopD = end * kOne;
        h.loopEndD = end * kOne;
        preroll = r.loopStart;
    }
    else
    {
        h.stopD = (1 - r.start) * kOne;
        h.loopEndD = (1 - r.loopStart) * kOne;
        preroll = frames - end;
    }
    const std::int64_t x = length / 2 < kSeamFrames ? length / 2 : kSeamFrames;
    h.xfade = x * kOne;
    h.xfadeSilent = preroll < x;
}

void wrapLoop(Head& h) noexcept
{
    if (h.d >= h.loopEndD)
    {
        const Fixed base = h.loopEndD - h.loopLen;
        h.d = base + (h.d - base) % h.loopLen;
    }
}

// m frames read at direction-normalised position d into out[c] + offset.
void readAt(const Head& h, Fixed d, float* const out[2], int offset, int m) noexcept
{
    const Fixed pos = h.dir > 0 ? d : -d;
    const Fixed step = h.dir > 0 ? h.step : -h.step;
    const int ch = channelsOf(h);
    const float* const* src = h.buf->ch;
    float* o[2] = {out[0] + offset, out[1] + offset};
    switch (h.interp)
    {
        case InterpolatorKind::Sinc16:
            kernels::sinc16Checked(src, ch, h.buf->frames, SampleBuffer::pad, pos, step, o, m);
            break;
        case InterpolatorKind::Linear: kernels::scalar::linear(src, ch, pos, step, o, m); break;
        case InterpolatorKind::Hermite4:
            if (h.step == kOne && static_cast<std::uint32_t>(pos) == 0)
                kernels::copy(src, ch, kernels::indexOf(pos), h.dir, o, m); // unpitched: the common drum case
            else
                kernels::hermite4(src, ch, pos, step, o, m);
            break;
    }
}

// The seam: outgoing at d, crossfaded into its continuation one loop length earlier (or into silence).
void readSeam(const Head& h, float* const out[2], float* const scratch[2], int offset, int m) noexcept
{
    readAt(h, h.d, out, offset, m);
    const Fixed zone = h.loopEndD - h.xfade;
    const float w0 = static_cast<float>(static_cast<double>(h.d - zone) / static_cast<double>(h.xfade));
    const float dw = static_cast<float>(static_cast<double>(h.step) / static_cast<double>(h.xfade));
    const int ch = channelsOf(h);
    if (h.xfadeSilent)
    {
        for (int c = 0; c < ch; ++c)
        {
            float* o = out[c] + offset;
            for (int j = 0; j < m; ++j)
                o[j] *= 1.0f - (w0 + static_cast<float>(j) * dw);
        }
        return;
    }
    readAt(h, h.d - h.loopLen, scratch, 0, m);
    for (int c = 0; c < ch; ++c)
    {
        float* o = out[c] + offset;
        const float* in = scratch[c];
        for (int j = 0; j < m; ++j)
            o[j] += (w0 + static_cast<float>(j) * dw) * (in[j] - o[j]);
    }
}
} // namespace

void beginHead(Head& h, const SampleBuffer* buf, const Region& region, Fixed step, InterpolatorKind interp) noexcept
{
    endHead(h);
    h = Head {};
    if (buf == nullptr || buf->frames <= 0 || step <= 0)
        return;
    const Region r = region.clamp(buf->frames);
    core::audio::retain(buf);
    h.buf = buf;
    h.dir = r.reverse ? -1 : 1;
    h.step = step;
    h.interp = interp;
    setGeometry(h, r);
    h.d = h.dir > 0 ? r.start * kOne : (1 - r.end()) * kOne;
    h.active = true;
}

void setHeadRegion(Head& h, const Region& region) noexcept
{
    if (! h.active)
        return;
    Region r = region.clamp(h.buf->frames);
    r.reverse = h.dir < 0;
    setGeometry(h, r);
}

void endHead(Head& h) noexcept
{
    if (h.buf != nullptr)
        core::audio::release(h.buf);
    h.buf = nullptr;
    h.active = false;
    h.fadeLeft = 0;
}

void fadeHead(Head& h, int frames) noexcept
{
    if (! h.active)
        return;
    frames = frames < 1 ? 1 : frames;
    if (h.fadeLeft > 0 && h.fadeLeft <= frames)
        return;
    h.fadeLeft = frames; // h.gain is the current gain (kept up to date while fading)
    h.delta = h.gain / static_cast<float>(frames);
}

void renderHead(Head& h, float* const out[2], int n, float* const scratch[2]) noexcept
{
    int k = 0;
    while (k < n && h.active)
    {
        int m;
        if (h.loop)
        {
            wrapLoop(h);
            const Fixed zone = h.loopEndD - h.xfade;
            if (h.xfade > 0 && h.d >= zone)
            {
                m = framesUntil(h.d, h.loopEndD, h.step, n - k);
                readSeam(h, out, scratch, k, m);
            }
            else
            {
                m = framesUntil(h.d, zone, h.step, n - k);
                readAt(h, h.d, out, k, m);
            }
            h.d += m * h.step;
            k += m;
            wrapLoop(h);
        }
        else
        {
            m = framesUntil(h.d, h.stopD, h.step, n - k);
            if (m == 0)
            {
                h.finished = true;
                break;
            }
            readAt(h, h.d, out, k, m);
            h.d += m * h.step;
            k += m;
            if (h.d >= h.stopD)
            {
                h.finished = true;
                break;
            }
        }
    }
    const bool mono = h.buf == nullptr || channelsOf(h) == 1;
    if (k < n)
    {
        std::memset(out[0] + k, 0, static_cast<std::size_t>(n - k) * sizeof(float));
        if (! mono)
            std::memset(out[1] + k, 0, static_cast<std::size_t>(n - k) * sizeof(float));
    }
    if (mono)
        std::memcpy(out[1], out[0], static_cast<std::size_t>(n) * sizeof(float));
}

void applyGain(Head& h, float* const out[2], int n) noexcept
{
    if (h.fadeLeft > 0)
    {
        const int m = h.fadeLeft < n ? h.fadeLeft : n;
        const float top = static_cast<float>(h.fadeLeft - 1), dg = h.delta; // frame j: (fadeLeft − 1 − j)·delta
        for (int c = 0; c < 2; ++c)
        {
            float* o = out[c];
            for (int j = 0; j < m; ++j)
                o[j] *= (top - static_cast<float>(j)) * dg;
            if (m < n)
                std::memset(o + m, 0, static_cast<std::size_t>(n - m) * sizeof(float));
        }
        h.fadeLeft -= m;
        h.gain = static_cast<float>(h.fadeLeft) * dg;
        if (h.fadeLeft == 0)
            endHead(h);
        return;
    }
    if (h.gain != 1.0f)
        for (int c = 0; c < 2; ++c)
            for (int j = 0; j < n; ++j)
                out[c][j] *= h.gain;
}

void addWithGain(Head& h, float* const dst[2], const float* const src[2], int n) noexcept
{
    if (h.fadeLeft <= 0)
    {
        const float g = h.gain;
        for (int c = 0; c < 2; ++c)
            for (int j = 0; j < n; ++j)
                dst[c][j] += src[c][j] * g;
        return;
    }
    const int m = h.fadeLeft < n ? h.fadeLeft : n;
    const float top = static_cast<float>(h.fadeLeft - 1), dg = h.delta; // frame j: (fadeLeft − 1 − j)·delta
    for (int c = 0; c < 2; ++c)
    {
        float* d = dst[c];
        const float* s = src[c];
        for (int j = 0; j < m; ++j)
            d[j] += s[j] * ((top - static_cast<float>(j)) * dg);
    }
    h.fadeLeft -= m;
    h.gain = static_cast<float>(h.fadeLeft) * dg;
    if (h.fadeLeft == 0)
        endHead(h);
}
} // namespace engine::voice
