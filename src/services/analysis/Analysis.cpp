#include "services/analysis/Analysis.h"

#include "services/analysis/PeakKernels.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace services::analysis
{
PeakOverview peaks(const float* const* ch, int channels, std::int64_t frames, int bucket)
{
    PeakOverview o;
    o.bucket = std::max(1, bucket);
    o.frames = std::max<std::int64_t>(0, frames);
    const auto count = static_cast<std::size_t>((o.frames + o.bucket - 1) / o.bucket);
    o.min.resize(count);
    o.max.resize(count);
    for (std::size_t b = 0; b < count; ++b)
    {
        const std::int64_t at = static_cast<std::int64_t>(b) * o.bucket;
        const int n = static_cast<int>(std::min<std::int64_t>(o.bucket, o.frames - at));
        float mn = std::numeric_limits<float>::infinity(), mx = -mn;
        for (int c = 0; c < channels; ++c)
            kernels::minMax(ch[c] + at, n, mn, mx);
        o.min[b] = mn;
        o.max[b] = mx;
    }
    return o;
}

PeakOverview peaks(const core::audio::SampleBuffer& s, int bucket)
{
    const int channels = std::clamp(static_cast<int>(s.channels), 1, 2);
    return peaks(s.ch, channels, s.frames, bucket);
}

double bpmFromBars(double bars, double seconds) noexcept
{
    return seconds > 0 && bars > 0 ? 240.0 * bars / seconds : 0.0;
}

double barsFromBpm(double bpm, double seconds) noexcept
{
    return bpm > 0 && seconds > 0 ? bpm * seconds / 240.0 : 0.0;
}

bool loopLength(double seconds) noexcept
{
    if (! (seconds > 0))
        return false;
    for (const int bars : {1, 2, 4, 8, 16})
    {
        // L(T) = 240·N / T over T ∈ [70, 180] spans [240N/180, 240N/70]; within 2 % of some L(T) ⇔
        // 0.98 · L(180) ≤ seconds ≤ 1.02 · L(70) (L is continuous in T).
        const double shortest = 240.0 * bars / 180.0, longest = 240.0 * bars / 70.0;
        if (seconds >= 0.98 * shortest && seconds <= 1.02 * longest)
            return true;
    }
    return false;
}

std::optional<SampleClass> classOverride(const Sidecar& m) noexcept
{
    if (const auto* e = m.extras.find(kClassOverrideKey))
    {
        if (e->value == "loop")
            return SampleClass::Loop;
        if (e->value == "oneshot")
            return SampleClass::OneShot;
    }
    return std::nullopt;
}

void setClassOverride(Sidecar& m, std::optional<SampleClass> c)
{
    auto& entries = m.extras.entries;
    if (! c)
    {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const auto& e) { return e.key == kClassOverrideKey; }),
                      entries.end());
        return;
    }
    m.extras.set(kClassOverrideKey, *c == SampleClass::Loop ? "loop" : "oneshot",
                 core::model::Extras::Encoding::Text);
}

SampleClass classify(std::int64_t frames, double sampleRate, const Sidecar& m) noexcept
{
    if (const auto o = classOverride(m))
        return *o;
    if (m.bars > 0)
        return SampleClass::Loop;
    const double seconds = sampleRate > 0 ? static_cast<double>(frames) / sampleRate : 0.0;
    return loopLength(seconds) ? SampleClass::Loop : SampleClass::OneShot;
}

SampleClass classify(const core::audio::SampleBuffer& s, const Sidecar& m) noexcept
{
    return classify(s.frames, s.sampleRate, m);
}

// ---------------------------------------------------------------------------------------------- PeakCache

juce::File PeakCache::fileFor(std::uint64_t contentId, int bucket) const
{
    return folder_.getChildFile(juce::String::toHexString(static_cast<juce::int64>(contentId)).paddedLeft('0', 16) +
                                "_" + juce::String(bucket) + ".peaks");
}

const PeakOverview& PeakCache::get(const core::audio::SampleBuffer& buffer, int bucket)
{
    const auto key = std::make_pair(buffer.contentId, bucket);
    if (const auto it = entries_.find(key); it != entries_.end())
        return *it->second;
    std::unique_ptr<PeakOverview> o;
    const bool disk = folder_ != juce::File();
    if (disk)
        if (auto r = read(fileFor(buffer.contentId, bucket)); r && r->frames == buffer.frames && r->bucket == bucket)
            o = std::make_unique<PeakOverview>(std::move(*r));
    if (! o)
    {
        o = std::make_unique<PeakOverview>(peaks(buffer, bucket));
        if (disk && folder_.createDirectory())
            write(fileFor(buffer.contentId, bucket), *o);
    }
    return *entries_.emplace(key, std::move(o)).first->second;
}

bool PeakCache::write(const juce::File& file, const PeakOverview& o)
{
    juce::MemoryOutputStream m;
    m.write("PKS1", 4);
    m.writeInt(o.bucket);
    m.writeInt64(o.frames);
    m.writeInt(static_cast<int>(o.min.size()));
    for (std::size_t i = 0; i < o.min.size(); ++i)
    {
        m.writeFloat(o.min[i]);
        m.writeFloat(o.max[i]);
    }
    const juce::File part = file.getSiblingFile(file.getFileName() + ".part");
    return part.replaceWithData(m.getData(), m.getDataSize()) && part.moveFileTo(file);
}

std::optional<PeakOverview> PeakCache::read(const juce::File& file)
{
    juce::MemoryBlock b;
    if (! file.existsAsFile() || file.getSize() > (64ll << 20) || ! file.loadFileAsData(b) || b.getSize() < 20)
        return std::nullopt;
    juce::MemoryInputStream in(b, false);
    char magic[4];
    if (in.read(magic, 4) != 4 || std::memcmp(magic, "PKS1", 4) != 0)
        return std::nullopt;
    PeakOverview o;
    o.bucket = in.readInt();
    o.frames = in.readInt64();
    const int count = in.readInt();
    // NFR-011: frames is bounded before the arithmetic (frames + bucket - 1 overflowed for a frames
    // field near INT64_MAX, signed overflow), and non-finite peaks are refused below (the waveform drawing casts them).
    if (o.bucket < 1 || o.frames < 0 || o.frames > (std::int64_t {1} << 40) || count < 0 ||
        static_cast<std::int64_t>(count) != (o.frames + o.bucket - 1) / o.bucket ||
        static_cast<std::size_t>(count) * 8 + 20 != b.getSize())
        return std::nullopt;
    o.min.resize(static_cast<std::size_t>(count));
    o.max.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const float lo = in.readFloat(), hi = in.readFloat();
        if (! std::isfinite(lo) || ! std::isfinite(hi))
            return std::nullopt; // a damaged cache file: recomputed by get()
        o.min[static_cast<std::size_t>(i)] = lo;
        o.max[static_cast<std::size_t>(i)] = hi;
    }
    return o;
}
} // namespace services::analysis
