// services::sampling — the take builder and SAVE SELECTION (spec 02-features/sampling.md SMP-006, SMP-009,
// SMP-022…025). Worker threads (or tests); nothing here runs on the audio thread.
//
//   buildTake    copies a live take's window plus one beat of margin on each side (for the nudge, SMP-024) out of
//                the capture ring into a new core::audio::SampleBuffer (refs 0, contentId = XXH64 of the PCM): buffer
//                frame k = ring frame (start − margin + k), so the window is [margin, margin + length) and a nudge n
//                is [margin + n, margin + n + length). The window itself must still be in the ring; margin frames the
//                ring does not hold (yet, or any more) are zero, and BuiltTake says how much of each margin is real.
//                Budget: 8 bars ≤ 10 ms on one A9 core (≈ 6 ms of copy at 120 BPM, 44.1 kHz).
//   proposeName  SMP-009: "<base> 001.wav", the first free running number in the folder.
//   saveBuffer   SAVE SELECTION: [from, to) of a buffer as a new 24-bit WAV at the buffer's rate, atomically (WavWriter
//                part file → rename), with a sidecar (bpm, bars).
//   saveRing     SMP-025 live Save: a ring window straight to a 24-bit WAV (no buffer needed; the ring holds ≥ 30 s).
#pragma once

#include "core/audio/SampleBuffer.h"
#include "core/audio/SampleBufferStorage.h"
#include "core/model/Sample.h"
#include "core/util/Result.h"
#include "core/util/sampling/CaptureHub.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>

namespace services::sampling
{
using TakeBuffer = std::unique_ptr<core::audio::SampleBuffer, core::audio::SampleBufferDeleter>;

struct TakeWindow
{
    std::int64_t start = 0, length = 0; // ring frames
    std::int32_t margin = 0;            // frames before and after the window
    int channels = 2;
    double sampleRate = 44100.0;
};

struct BuiltTake
{
    TakeBuffer buffer;
    std::int32_t margin = 0;      // the buffer's layout margin (= TakeWindow::margin)
    std::int64_t preCaptured = 0; // frames of the leading margin that hold audio (the rest is zero)
    std::int64_t postCaptured = 0; // frames of the trailing margin that hold audio
};

core::Result<BuiltTake> buildTake(const core::sampling::CaptureRing&, const TakeWindow&);

juce::File proposeName(const juce::File& folder, const juce::String& base = "REC");

core::Result<juce::File> saveBuffer(const core::audio::SampleBuffer&, std::int64_t from, std::int64_t to,
                                    const juce::File& target, const core::model::SampleMetadata&);
core::Result<juce::File> saveRing(const core::sampling::CaptureRing&, std::int64_t from, std::int64_t frames,
                                  int channels, double sampleRate, const juce::File& target,
                                  const core::model::SampleMetadata&);
} // namespace services::sampling
