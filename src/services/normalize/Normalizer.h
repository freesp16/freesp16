// Exact peak normalize (spec 02-features/render-and-processing.md REN-010…014, REN-016).
//
//   REN-010  the single maximum |x| over the whole file and all channels; one exact gain (double) for every sample of
//            every channel; round to nearest, ties to even; no dither. PCM: the gain is ceiling / peak in integer
//            counts, the product x · gain is rounded with nearbyint and an exact integer tie-break when the product
//            lies within 1e-6 of a half (so a rational tie x · C / P = k + ½ goes to the even k even where the double
//            product misses the tie by one ulp). 32-bit float: y = float(x · (T / P)), the float conversion rounds to
//            nearest even.
//   REN-011  target −6…0 dBFS, default −1.0. PCM ceiling = ⌊2^(bits−1) · 10^(target/20)⌋, at most 2^(bits−1) − 1:
//            29204 (PCM16) and 7476354 (PCM24) at −1 dBFS. Float target = 10^(target/20).
//   REN-012  exact digital silence and a file whose peak already equals the target are left unchanged: nothing is
//            written, NormalizeReport::changed = false (the caller keeps the original; REN-014's Keep has
//            nothing to repoint).
//   REN-013  PCM16 / PCM24 / 32-bit float, WAV or AIFF, mono or stereo, 44.1 / 48 / 96 kHz. Anything else (8-bit,
//            32-bit integer, 64-bit float, > 2 channels, other rates, unreadable files) is refused with a message
//            before anything is written. The output has the input's encoding, channels and rate; its container
//            follows `out`'s extension (.aif / .aiff = AIFF, else WAV). It is written to a temporary sibling and
//            renamed into place, so a failed write leaves no partial file.
//   REN-014  Keep / Discard: normalizedFileFor() names `<name>_norm.wav`; the caller repoints the track with an undoable
//            load (services::library::TrackLoader) on Keep, or deletes the file on Discard.
//
// Two streaming passes of 64 k frames (peak, then gain + write): memory stays ≈ 1 MB for any length. Worker threads
// (any thread but audio). Budget: 1 min stereo PCM24 ≤ 1 s on the device (bench_render_offline "normalize").
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <span>

namespace services::normalize
{
enum class Encoding : std::uint8_t { Pcm16, Pcm24, Float32 };

struct NormalizeReport
{
    Encoding encoding = Encoding::Pcm16;
    int channels = 0;
    double sampleRate = 0;
    std::int64_t frames = 0;
    double targetDbfs = -1.0;
    double peakBefore = 0;      // max |x|: integer counts (PCM) or full-scale float
    double peakAfter = 0;       // the same after the gain (= the ceiling / target when changed)
    double gain = 1.0;          // the one gain applied
    std::int64_t ceiling = 0;   // PCM: the integer target peak
    bool silence = false;       // REN-012: every sample 0
    bool atTarget = false;      // REN-012: the peak already equals the target
    bool changed = false;       // true = `out` was written
};

// REN-010…013. `out` must differ from `in`.
core::Result<NormalizeReport> normalize(const juce::File& in, const juce::File& out, double targetDbfs = -1.0);

// ---- the pieces (tests, the render service's in-memory path) ----

// ⌊2^(bits−1) · 10^(dbfs/20)⌋ clamped to 2^(bits−1) − 1 (REN-011). bits = 16 or 24.
std::int64_t pcmCeiling(int bits, double dbfs) noexcept;
// x · gain rounded to nearest, ties to even, with the exact tie-break of the header (x · ceiling / peak).
std::int32_t scalePcm(std::int32_t x, double gain, std::int64_t ceiling, std::int64_t peak) noexcept;
// In place over integer counts (not left-justified). peak = max |x| of the whole file, > 0.
void scalePcmBlock(std::span<std::int32_t> x, std::int64_t ceiling, std::int64_t peak) noexcept;
// Max |x| of integer counts / floats.
std::int64_t peakOf(std::span<const std::int32_t>) noexcept;
float peakOf(std::span<const float>) noexcept;

// REN-014: kick.wav → kick_norm.wav (same folder; WAV).
juce::File normalizedFileFor(const juce::File& in);
} // namespace services::normalize
