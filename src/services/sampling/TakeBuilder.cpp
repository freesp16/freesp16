#include "services/sampling/TakeBuilder.h"

#include "services/library/Hash.h"
#include "services/library/SidecarStore.h"
#include "services/sampling/WavWriter.h"

#include <algorithm>
#include <vector>

namespace services::sampling
{
core::Result<BuiltTake> buildTake(const core::sampling::CaptureRing& ring, const TakeWindow& w)
{
    using R = core::Result<BuiltTake>;
    if (w.length <= 0 || w.margin < 0)
        return R::failure("Nothing to build", "empty take window");
    const int ch = w.channels == 1 ? 1 : 2;
    const std::int64_t m = w.margin;
    const std::int64_t frames = w.length + 2 * m;
    TakeBuffer buffer(core::audio::allocateSampleBuffer(frames, ch, w.sampleRate));
    if (! buffer)
        return R::failure("Not enough memory for the take", "allocateSampleBuffer");
    // What the ring holds of [start − m, end + m): the window itself is required.
    const std::int64_t lo = w.start - m, hi = w.start + w.length + m;
    const std::int64_t from = std::max({lo, ring.oldest(), std::int64_t {0}});
    const std::int64_t to = std::min(hi, ring.head());
    if (from > w.start || to < w.start + w.length)
        return R::failure("The take is no longer in the capture ring", "window outside [oldest, head)");
    float* l = core::audio::writableChannel(buffer.get(), 0);
    float* r = ch == 2 ? core::audio::writableChannel(buffer.get(), 1) : nullptr;
    const std::int64_t at = from - lo;
    if (! ring.read(from, static_cast<int>(to - from), l + at, r != nullptr ? r + at : nullptr))
        return R::failure("The take is no longer in the capture ring", "overwritten while copying");
    library::Xxh64 h(0x54414b45u ^ static_cast<std::uint64_t>(ch)); // "TAKE"
    h.update(l, sizeof(float) * static_cast<std::size_t>(frames));
    if (r != nullptr)
        h.update(r, sizeof(float) * static_cast<std::size_t>(frames));
    buffer->contentId = h.digest();
    BuiltTake out;
    out.margin = w.margin;
    out.preCaptured = w.start - from;
    out.postCaptured = to - (w.start + w.length);
    out.buffer = std::move(buffer);
    return out;
}

juce::File proposeName(const juce::File& folder, const juce::String& base)
{
    for (int n = 1; n < 100000; ++n)
    {
        const auto f = folder.getChildFile(base + " " + juce::String(n).paddedLeft('0', 3) + ".wav");
        if (! f.exists() && ! WavWriter::partFor(f).exists())
            return f;
    }
    return folder.getNonexistentChildFile(base, ".wav", false);
}

namespace
{
core::Result<juce::File> finish(WavWriter& w, const juce::File& target, const core::model::SampleMetadata& md)
{
    if (auto ok = w.finish(); ! ok)
        return ok.error();
    if (auto ok = library::SidecarStore().write(target, md); ! ok)
        return ok.error();
    return target;
}
} // namespace

core::Result<juce::File> saveBuffer(const core::audio::SampleBuffer& b, std::int64_t from, std::int64_t to,
                                    const juce::File& target, const core::model::SampleMetadata& md)
{
    from = std::clamp<std::int64_t>(from, 0, b.frames);
    to = std::clamp<std::int64_t>(to, from, b.frames);
    if (to <= from)
        return core::Result<juce::File>::failure("The selection is empty", "from >= to");
    WavWriter w;
    if (auto ok = w.open(target, b.sampleRate, b.channels); ! ok)
        return ok.error();
    if (auto ok = w.write(b.ch[0] + from, b.ch[1] + from, to - from); ! ok)
        return ok.error();
    return finish(w, target, md);
}

core::Result<juce::File> saveRing(const core::sampling::CaptureRing& ring, std::int64_t from, std::int64_t frames,
                                  int channels, double sampleRate, const juce::File& target,
                                  const core::model::SampleMetadata& md)
{
    if (frames <= 0)
        return core::Result<juce::File>::failure("The take is empty", "frames <= 0");
    WavWriter w;
    if (auto ok = w.open(target, sampleRate, channels); ! ok)
        return ok.error();
    constexpr std::int64_t kChunk = 16384;
    std::vector<float> l(kChunk), r(kChunk);
    for (std::int64_t done = 0; done < frames;)
    {
        const int n = static_cast<int>(std::min(kChunk, frames - done));
        if (! ring.read(from + done, n, l.data(), r.data()))
        {
            w.abandon();
            return core::Result<juce::File>::failure("The take is no longer in the capture ring", "saveRing");
        }
        if (auto ok = w.write(l.data(), r.data(), n); ! ok)
            return ok.error();
        done += n;
    }
    return finish(w, target, md);
}
} // namespace services::sampling
