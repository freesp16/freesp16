// Golden harness (spec 05-quality/testing-strategy.md "DSP goldens from the research models"): loads a
// golden case written by tools/golden-gen/gen.py (<case>.json + <case>.in.f32 + <case>.out.f32), runs a Subject over
// its input in 64-frame blocks and compares the output with the model's.
//
// Tolerance (testing-strategy.md "Tolerances", default row):
//   per channel, max |actual - expected| <= max_rel x peak(expected)   (peak of the channel; of the case if silent)
//   and the null-test residual RMS(actual - expected) <= residual_dbfs  (0 dBFS = 1.0)
//   samples where both |actual| and |expected| are below `floor` (1e-30) count as equal: the app runs with FTZ on,
//   the reference with FTZ off (FX-055), so denormal-range tails may differ; `window` limits the comparison.
// Parity mode (optional, `--parity`): bit-exact float32 on every sample, except values that are both below FLT_MIN.
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace golden
{
using Channels = std::vector<std::vector<float>>;

struct Tolerance
{
    double maxRel = 1e-5;
    double residualDbfs = -100.0;
    double floor = 1e-30;
};

struct GoldenCase
{
    std::string name, id, subject, description;
    double sampleRate = 44100.0;
    int block = 64, blocks = 0, frames = 0;
    int windowBegin = 0, windowEnd = 0;
    bool parityExpected = false;
    Tolerance tolerance;
    juce::var params; // subject-specific (see the model adapter in tools/golden-gen)
    std::vector<std::string> inputNames, outputNames;
    Channels input, expected;
    std::vector<std::uint32_t> gainLawBits; // the app gain table the model ran with (empty: none plugged in)

    std::string label() const { return name + "/" + id; }
};

// Loads <dir>/<id>.json and its two sample files. Returns "" or a readable error.
std::string loadCase(const std::filesystem::path& json, GoldenCase& out);

// "" when the golden's plugged gain table equals core::GainLaw::gain() bit for bit, else a readable report.
std::string checkGainLaw(const GoldenCase& c);

struct CompareResult
{
    bool pass = false;
    std::string report; // multi-line, starts with "GOLDEN PASS" or "GOLDEN FAIL"
};

CompareResult compare(const GoldenCase& c, const Channels& actual, bool parity, const std::string& mode = {});

// Writes channel-major little-endian float32 (the golden file format). Returns false on an I/O error.
bool writeF32(const std::filesystem::path& file, const Channels& channels);
} // namespace golden
