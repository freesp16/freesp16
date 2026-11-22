#include "tests/golden/harness/GoldenCase.h"

#include "core/util/GainLaw.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace golden
{
static_assert(std::endian::native == std::endian::little, "golden .f32 files are little-endian");

namespace
{
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::vector<std::string> strings(const juce::var& v)
{
    std::vector<std::string> out;
    if (auto* a = v.getArray())
        for (auto& s : *a)
            out.push_back(s.toString().toStdString());
    return out;
}

std::string readF32(const std::filesystem::path& file, std::size_t channels, int frames, Channels& out)
{
    std::ifstream in(file, std::ios::binary);
    if (! in)
        return "cannot open " + file.string() + " (generate it: tools/golden-gen/gen.py)";
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t want = channels * static_cast<std::size_t>(frames) * sizeof(float);
    if (bytes.size() != want)
        return format("%s: %zu bytes, expected %zu (%zu channels x %d frames)", file.string().c_str(), bytes.size(),
                      want, channels, frames);
    out.assign(channels, std::vector<float>(static_cast<std::size_t>(frames)));
    for (std::size_t c = 0; c < channels; ++c)
        std::memcpy(out[c].data(), bytes.data() + c * static_cast<std::size_t>(frames) * sizeof(float),
                    static_cast<std::size_t>(frames) * sizeof(float));
    return {};
}

double dbfs(double x)
{
    return x > 0.0 ? 20.0 * std::log10(x) : -std::numeric_limits<double>::infinity();
}

std::string where(int frame, int block)
{
    return format("frame %d (block %d, offset %d)", frame, frame / block, frame % block);
}

bool tiny(float x)
{
    return std::fabs(x) < FLT_MIN;
}
} // namespace

std::string loadCase(const std::filesystem::path& json, GoldenCase& c)
{
    juce::File f(juce::String(std::filesystem::absolute(json).string()));
    if (! f.existsAsFile())
        return "golden case missing: " + json.string() + " (generate it: tools/golden-gen/gen.py)";
    juce::var doc;
    const auto parsed = juce::JSON::parse(f.loadFileAsString(), doc);
    if (parsed.failed())
        return json.string() + ": " + parsed.getErrorMessage().toStdString();
    if (doc["format"].toString() != "golden-case/1")
        return json.string() + ": unknown format '" + doc["format"].toString().toStdString() + "'";
    c.name = doc["name"].toString().toStdString();
    c.id = doc["case"].toString().toStdString();
    c.subject = doc["subject"].toString().toStdString();
    c.description = doc["description"].toString().toStdString();
    c.sampleRate = static_cast<double>(doc["sample_rate"]);
    c.block = static_cast<int>(doc["block"]);
    c.blocks = static_cast<int>(doc["blocks"]);
    c.frames = static_cast<int>(doc["frames"]);
    if (c.block != 64 || c.frames != c.blocks * c.block)
        return json.string() + ": block must be 64 and frames = blocks x 64";
    const auto& tol = doc["tolerance"];
    c.tolerance.maxRel = static_cast<double>(tol["max_rel"]);
    c.tolerance.residualDbfs = static_cast<double>(tol["residual_dbfs"]);
    c.tolerance.floor = static_cast<double>(tol["floor"]);
    c.windowBegin = 0;
    c.windowEnd = c.frames;
    if (auto* w = doc["window"].getArray(); w != nullptr && w->size() == 2)
    {
        c.windowBegin = std::clamp(static_cast<int>((*w)[0]), 0, c.frames);
        c.windowEnd = std::clamp(static_cast<int>((*w)[1]), c.windowBegin, c.frames);
    }
    c.parityExpected = static_cast<bool>(doc["parity"]);
    c.params = doc["params"];
    c.inputNames = strings(doc["input"]["channels"]);
    c.outputNames = strings(doc["expected"]["channels"]);
    c.gainLawBits.clear();
    if (auto* bits = doc["tables"]["gain_law"]["f32_bits"].getArray())
        for (auto& b : *bits)
            c.gainLawBits.push_back(static_cast<std::uint32_t>(static_cast<juce::int64>(b)));
    const auto dir = json.parent_path();
    if (auto e = readF32(dir / doc["input"]["file"].toString().toStdString(), c.inputNames.size(), c.frames, c.input);
        ! e.empty())
        return e;
    return readF32(dir / doc["expected"]["file"].toString().toStdString(), c.outputNames.size(), c.frames, c.expected);
}

std::string checkGainLaw(const GoldenCase& c)
{
    if (c.gainLawBits.empty())
        return {};
    if (c.gainLawBits.size() != static_cast<std::size_t>(core::GainLaw::maxIndex + 1))
        return format("GOLDEN FAIL %s: gain table has %zu entries, GainLaw has %d", c.label().c_str(),
                      c.gainLawBits.size(), core::GainLaw::maxIndex + 1);
    std::string r;
    int bad = 0;
    for (int i = 0; i <= core::GainLaw::maxIndex; ++i)
    {
        const double app = static_cast<double>(core::GainLaw::gain(i));
        const double gold = static_cast<double>(std::bit_cast<float>(c.gainLawBits[static_cast<std::size_t>(i)]));
        if (std::bit_cast<std::uint32_t>(core::GainLaw::gain(i)) != c.gainLawBits[static_cast<std::size_t>(i)] &&
            bad++ < 8)
            r += format("  index %3d: golden %.9g (%.4f dB), core::GainLaw::gain %.9g (%.4f dB)\n", i, gold, dbfs(gold),
                        app, dbfs(app));
    }
    if (bad == 0)
        return {};
    return format("GOLDEN FAIL %s: the gain table the model ran with differs from core/util/GainLaw.h in %d entries\n",
                  c.label().c_str(), bad) +
           r +
           "  -> GainLaw.h changed (or tools/golden-gen/goldenlib/applaw.py no longer mirrors it): update applaw.py, "
           "regenerate with tools/golden-gen/gen.py " +
           c.name + " and review the diff\n";
}

CompareResult compare(const GoldenCase& c, const Channels& actual, bool parity, const std::string& mode)
{
    CompareResult res;
    const std::string title = c.label() + (mode.empty() ? "" : " [" + mode + "]");
    if (actual.size() != c.expected.size())
    {
        res.report = format("GOLDEN FAIL %s: subject produced %zu channels, the golden has %zu\n", title.c_str(),
                            actual.size(), c.expected.size());
        return res;
    }
    const auto& t = c.tolerance;
    double casePeak = 0.0;
    for (const auto& ch : c.expected)
        for (int n = c.windowBegin; n < c.windowEnd; ++n)
            casePeak = std::max(casePeak, static_cast<double>(std::fabs(ch[static_cast<std::size_t>(n)])));
    const int count = c.windowEnd - c.windowBegin;
    bool pass = true;
    std::string body;
    double worstAbs = 0.0, worstResidual = -std::numeric_limits<double>::infinity();
    for (std::size_t ch = 0; ch < c.expected.size(); ++ch)
    {
        const auto& x = c.expected[ch];
        const auto& a = actual[ch];
        const std::string& cname = ch < c.outputNames.size() ? c.outputNames[ch] : std::to_string(ch);
        double peak = 0.0;
        for (int n = c.windowBegin; n < c.windowEnd; ++n)
            peak = std::max(peak, static_cast<double>(std::fabs(x[static_cast<std::size_t>(n)])));
        const double limit = t.maxRel * (peak > 0.0 ? peak : casePeak);
        double maxAbs = 0.0, sumSq = 0.0;
        int argMax = -1, firstOver = -1, over = 0, firstBits = -1, bitDiffs = 0, nonFinite = -1;
        for (int n = c.windowBegin; n < c.windowEnd; ++n)
        {
            const float xv = x[static_cast<std::size_t>(n)], av = a[static_cast<std::size_t>(n)];
            if (! std::isfinite(av))
            {
                nonFinite = n;
                break;
            }
            double e = static_cast<double>(av) - static_cast<double>(xv);
            if (std::fabs(static_cast<double>(xv)) < t.floor && std::fabs(static_cast<double>(av)) < t.floor)
                e = 0.0;
            const double ae = std::fabs(e);
            sumSq += e * e;
            if (ae > maxAbs)
            {
                maxAbs = ae;
                argMax = n;
            }
            if (ae > limit)
            {
                if (firstOver < 0)
                    firstOver = n;
                ++over;
            }
            const bool same =
                std::bit_cast<std::uint32_t>(av) == std::bit_cast<std::uint32_t>(xv) || (tiny(av) && tiny(xv));
            if (! same)
            {
                if (firstBits < 0)
                    firstBits = n;
                ++bitDiffs;
            }
        }
        if (nonFinite >= 0)
        {
            pass = false;
            body +=
                format("  %-8s non-finite output %g at %s\n", cname.c_str(),
                       static_cast<double>(a[static_cast<std::size_t>(nonFinite)]), where(nonFinite, c.block).c_str());
            continue;
        }
        const double residual = dbfs(count > 0 ? std::sqrt(sumSq / count) : 0.0);
        worstAbs = std::max(worstAbs, maxAbs);
        worstResidual = std::max(worstResidual, residual);
        const bool chPass = parity ? bitDiffs == 0 : (over == 0 && residual <= t.residualDbfs);
        if (chPass)
            continue;
        pass = false;
        body += format("  %-8s max |err| %.3g (%.1f dBFS, %.3g x peak %.4g) at %s: expected %.9g, got %.9g\n",
                       cname.c_str(), maxAbs, dbfs(maxAbs), peak > 0.0 ? maxAbs / peak : 0.0, peak,
                       where(argMax, c.block).c_str(), static_cast<double>(x[static_cast<std::size_t>(argMax)]),
                       static_cast<double>(a[static_cast<std::size_t>(argMax)]));
        body += format("  %-8s residual %.1f dBFS (limit %.1f); %d of %d frames over %.3g", "", residual,
                       t.residualDbfs, over, count, limit);
        if (firstOver >= 0)
            body += format(", first at %s: expected %.9g, got %.9g", where(firstOver, c.block).c_str(),
                           static_cast<double>(x[static_cast<std::size_t>(firstOver)]),
                           static_cast<double>(a[static_cast<std::size_t>(firstOver)]));
        body += "\n";
        if (parity && firstBits >= 0)
            body += format("  %-8s parity: %d of %d samples not bit-exact, first at %s: expected 0x%08x, got 0x%08x\n",
                           "", bitDiffs, count, where(firstBits, c.block).c_str(),
                           std::bit_cast<std::uint32_t>(x[static_cast<std::size_t>(firstBits)]),
                           std::bit_cast<std::uint32_t>(a[static_cast<std::size_t>(firstBits)]));
    }
    res.pass = pass;
    const std::string crit = parity
                                 ? std::string("bit-exact (parity mode)")
                                 : format("max |err| <= %.3g x peak, residual <= %.1f dBFS", t.maxRel, t.residualDbfs);
    if (pass)
    {
        res.report =
            format("GOLDEN PASS %s: %zu channels x %d frames (%s); worst max |err| %.3g (%.1f dBFS), worst "
                   "residual %.1f dBFS\n",
                   title.c_str(), c.expected.size(), count, crit.c_str(), worstAbs, dbfs(worstAbs), worstResidual);
        return res;
    }
    res.report = format("GOLDEN FAIL %s (subject %s, frames %d..%d, %s)\n", title.c_str(), c.subject.c_str(),
                        c.windowBegin, c.windowEnd, crit.c_str()) +
                 (c.description.empty() ? "" : "  case: " + c.description + "\n") + body;
    return res;
}

bool writeF32(const std::filesystem::path& file, const Channels& channels)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    for (const auto& ch : channels)
        out.write(reinterpret_cast<const char*>(ch.data()), static_cast<std::streamsize>(ch.size() * sizeof(float)));
    return static_cast<bool>(out);
}
} // namespace golden
