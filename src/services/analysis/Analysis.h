// Sample analysis (spec 02-features/render-and-processing.md REN-020, REN-021, REN-024).
//
//   REN-020  peaks(): min / max over all channels per `bucket` frames (256), NEON kernel with a scalar reference
//            (PeakKernels.h). PeakCache keeps them per content id (SampleBuffer::contentId) in RAM and, when given a
//            folder, as `<contentId>_<bucket>.peaks` files there (the app's cache folder; the library's
//            SidecarStore writes only the sample's `.json` sidecar, so the peak cache is its own file next to the
//            other caches). Budget ≤ 0.3 s per minute of audio on the device (bench_render_offline "peaks").
//   REN-021  bars ⇄ BPM from the sample length (4/4): bpm = 240 · bars / seconds, bars = bpm · seconds / 240.
//   REN-024  classify(): a loop is a sample whose sidecar has bars > 0, or whose length lies within 2 % of a whole
//            number of bars (1, 2, 4, 8, 16) at some tempo in 70–180 BPM (|L − 240·N/T| ≤ 0.02 · 240·N/T); anything
//            else is a one-shot. A per-sample override (sidecar extras "analysis.class": "loop" / "oneshot", ours)
//            wins. [contract] SidecarStore::toJson does not write extras yet, so the override lives as long as the
//            in-memory metadata; persisting it is a one-line addition to the library's sidecar JSON (I-item).
//
// Pure functions, any thread but audio (PeakCache: one owner thread).
#pragma once

#include "core/audio/SampleBuffer.h"
#include "core/model/Sample.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace services::analysis
{
using Sidecar = core::model::SampleMetadata;

struct PeakOverview
{
    int bucket = 256;
    std::int64_t frames = 0;
    std::vector<float> min, max; // one entry per bucket (the last may be partial); over all channels
    friend bool operator== (const PeakOverview&, const PeakOverview&) = default;
};

PeakOverview peaks(const core::audio::SampleBuffer&, int bucket = 256);
// The same over planar channels (tests, the render service's output).
PeakOverview peaks(const float* const* ch, int channels, std::int64_t frames, int bucket = 256);

double bpmFromBars(double bars, double seconds) noexcept; // REN-021: 240·bars / seconds (0 when seconds ≤ 0)
double barsFromBpm(double bpm, double seconds) noexcept;  // REN-021: bpm · seconds / 240

enum class SampleClass : std::uint8_t { OneShot, Loop };

SampleClass classify(const core::audio::SampleBuffer&, const Sidecar&) noexcept; // REN-024
SampleClass classify(std::int64_t frames, double sampleRate, const Sidecar&) noexcept;
// The length rule alone (no sidecar, no override).
bool loopLength(double seconds) noexcept;

inline constexpr const char* kClassOverrideKey = "analysis.class";
std::optional<SampleClass> classOverride(const Sidecar&) noexcept;
void setClassOverride(Sidecar&, std::optional<SampleClass>);

class PeakCache
{
public:
    explicit PeakCache(juce::File folder = {}) : folder_(std::move(folder)) {}
    // The overview of `buffer`, computed once per (contentId, bucket).
    const PeakOverview& get(const core::audio::SampleBuffer& buffer, int bucket = 256);
    std::size_t size() const noexcept { return entries_.size(); }
    void clear() { entries_.clear(); }

    // File format (little-endian): "PKS1", int32 bucket, int64 frames, int32 count, count × (float min, float max).
    static bool write(const juce::File&, const PeakOverview&);
    static std::optional<PeakOverview> read(const juce::File&);

private:
    juce::File fileFor(std::uint64_t contentId, int bucket) const;
    juce::File folder_;
    std::map<std::pair<std::uint64_t, int>, std::unique_ptr<PeakOverview>> entries_;
};
} // namespace services::analysis
