#include "services/library/SidecarStore.h"

#include "services/store/AtomicWriter.h"

#include <cmath>
#include <cstring>
#include <string_view>

namespace services::library
{
namespace
{
using core::model::SampleMetadata;

constexpr int kMaxDepth = 8;
constexpr int kMaxEntries = 256;
constexpr std::size_t kMaxString = 4096;

// A cursor over untrusted bytes: every read is bounds-checked, a failed read poisons the cursor.
class Cursor
{
public:
    Cursor(const unsigned char* p, std::size_t n) : p_(p), n_(n) {}
    bool ok() const noexcept { return ok_; }
    bool byte(unsigned char& out) noexcept
    {
        if (! ok_ || at_ >= n_)
            return ok_ = false;
        out = p_[at_++];
        return true;
    }
    bool bytes(std::size_t count, const unsigned char*& out) noexcept
    {
        if (! ok_ || count > n_ - at_)
            return ok_ = false;
        out = p_ + at_;
        at_ += count;
        return true;
    }
    // JUCE OutputStream::writeCompressedInt: size byte (bit 7 = negative, bits 0–6 = 0…4 bytes), then LE bytes.
    bool compressedInt(std::int64_t& out) noexcept
    {
        unsigned char size = 0;
        if (! byte(size))
            return false;
        const int count = size & 0x7F;
        if (count > 4)
            return ok_ = false;
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i)
        {
            unsigned char b = 0;
            if (! byte(b))
                return false;
            v |= static_cast<std::uint32_t>(b) << (8 * i);
        }
        out = (size & 0x80) != 0 ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
        return true;
    }
    // NUL-terminated UTF-8 (JUCE writeString).
    bool string(std::string_view& out) noexcept
    {
        const std::size_t start = at_;
        while (ok_ && at_ < n_ && p_[at_] != 0)
            ++at_;
        if (! ok_ || at_ >= n_ || at_ - start > kMaxString)
            return ok_ = false;
        out = std::string_view(reinterpret_cast<const char*>(p_ + start), at_ - start);
        ++at_;
        return true;
    }

private:
    const unsigned char* p_;
    std::size_t n_, at_ = 0;
    bool ok_ = true;
};

enum Marker : unsigned char
{
    Int = 1,
    True = 2,
    False = 3,
    Double = 4,
    String = 5,
    Int64 = 6,
    Array = 7,
    Binary = 8
};

struct Number
{
    bool present = false;
    double value = 0;
};

// One var: compressed size (1 + payload), marker, payload. Numbers are returned, everything else skipped.
bool readVar(Cursor& c, Number& out)
{
    std::int64_t size = 0;
    if (! c.compressedInt(size))
        return false;
    if (size <= 0)
        return true; // void
    unsigned char marker = 0;
    const unsigned char* payload = nullptr;
    if (! c.byte(marker) || ! c.bytes(static_cast<std::size_t>(size - 1), payload))
        return false;
    const auto n = static_cast<std::size_t>(size - 1);
    if (marker == Int && n == 4)
    {
        std::int32_t v;
        std::memcpy(&v, payload, 4);
        out = {true, static_cast<double>(v)};
    }
    else if (marker == Int64 && n == 8)
    {
        std::int64_t v;
        std::memcpy(&v, payload, 8);
        out = {true, static_cast<double>(v)};
    }
    else if (marker == Double && n == 8)
    {
        double v;
        std::memcpy(&v, payload, 8);
        if (std::isfinite(v))
            out = {true, v};
    }
    return true; // strings, arrays, binaries: skipped whole by their declared size
}

bool readNode(Cursor& c, int depth, SampleMetadata* root, bool& isAdjunct)
{
    if (depth > kMaxDepth)
        return false;
    std::string_view type;
    if (! c.string(type))
        return false;
    if (root != nullptr)
        isAdjunct = type == "SampleAdjunctData";
    std::int64_t props = 0;
    if (! c.compressedInt(props) || props < 0 || props > kMaxEntries)
        return false;
    for (std::int64_t i = 0; i < props; ++i)
    {
        std::string_view name;
        Number value;
        if (! c.string(name) || ! readVar(c, value))
            return false;
        if (root == nullptr || ! value.present)
            continue;
        if (name == "bar" || name == "bars")
            root->bars = value.value;
        else if (name == "bpm")
            root->bpm = value.value;
        else if (name == "sampleNum" || name == "sampleNumber")
            root->sampleNumber = static_cast<int>(std::clamp(value.value, -2147483648.0, 2147483647.0));
    }
    std::int64_t children = 0;
    if (! c.compressedInt(children) || children < 0 || children > kMaxEntries)
        return false;
    for (std::int64_t i = 0; i < children; ++i)
        if (! readNode(c, depth + 1, nullptr, isAdjunct))
            return false;
    return true;
}

double sane(double v, double lo, double hi)
{
    return std::isfinite(v) && v >= lo && v <= hi ? v : 0.0;
}
} // namespace

juce::File SidecarStore::jsonFor(const juce::File& audio)
{
    return audio.getSiblingFile(audio.getFileName() + ".json");
}

juce::File SidecarStore::referenceFor(const juce::File& audio)
{
    return audio.getSiblingFile(audio.getFileName() + ".dat");
}

std::optional<SampleMetadata> SidecarStore::parseReference(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0 || size > kMaxSidecarBytes)
        return std::nullopt;
    Cursor c(static_cast<const unsigned char*>(data), size);
    SampleMetadata m;
    bool adjunct = false;
    if (! readNode(c, 0, &m, adjunct) || ! adjunct)
        return std::nullopt;
    m.bars = sane(m.bars, 0, 100000);
    m.bpm = sane(m.bpm, 0, 1000);
    return m;
}

std::optional<SampleMetadata> SidecarStore::parseJson(const juce::String& text)
{
    if (text.getNumBytesAsUTF8() > kMaxSidecarBytes)
        return std::nullopt;
    juce::var v;
    if (juce::JSON::parse(text, v).failed() || ! v.isObject())
        return std::nullopt;
    auto number = [&v](const char* key, double& out)
    {
        const juce::var x = v.getProperty(key, {});
        if (x.isDouble() || x.isInt() || x.isInt64())
            out = static_cast<double>(x);
    };
    SampleMetadata m;
    double sampleNumber = 0, peak = 0, rms = 0, loudness = 0;
    number("bars", m.bars);
    number("bpm", m.bpm);
    number("sampleNumber", sampleNumber);
    number("peak", peak);
    number("rms", rms);
    number("loudness", loudness);
    m.bars = sane(m.bars, 0, 100000);
    m.bpm = sane(m.bpm, 0, 1000);
    m.sampleNumber = static_cast<int>(sane(sampleNumber, -2147483648.0, 2147483647.0));
    m.peak = static_cast<float>(sane(peak, 0, 1000));
    m.rms = static_cast<float>(sane(rms, 0, 1000));
    m.loudness = static_cast<float>(sane(loudness, -1000, 1000));
    return m;
}

juce::String SidecarStore::toJson(const SampleMetadata& m)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("format", "sample-meta");
    o->setProperty("version", 1);
    o->setProperty("bars", m.bars);
    o->setProperty("bpm", m.bpm);
    o->setProperty("sampleNumber", m.sampleNumber);
    if (m.peak != 0 || m.rms != 0 || m.loudness != 0)
    {
        o->setProperty("peak", static_cast<double>(m.peak));
        o->setProperty("rms", static_cast<double>(m.rms));
        o->setProperty("loudness", static_cast<double>(m.loudness));
    }
    return juce::JSON::toString(juce::var(o), juce::JSON::FormatOptions{}.withSpacing(juce::JSON::Spacing::multiLine));
}

bool SidecarStore::tryRead(const juce::File& audio, SampleMetadata& out) const
{
    const juce::File json = jsonFor(audio);
    if (json.existsAsFile() && json.getSize() <= static_cast<juce::int64>(kMaxSidecarBytes))
        if (auto m = parseJson(json.loadFileAsString()))
        {
            out = *m;
            return true;
        }
    const juce::File dat = referenceFor(audio);
    if (dat.existsAsFile() && dat.getSize() <= static_cast<juce::int64>(kMaxSidecarBytes))
    {
        juce::MemoryBlock block;
        if (dat.loadFileAsData(block))
            if (auto m = parseReference(block.getData(), block.getSize()))
            {
                out = *m;
                return true;
            }
    }
    return false;
}

SampleMetadata SidecarStore::read(const juce::File& audio) const
{
    SampleMetadata m;
    tryRead(audio, m);
    return m;
}

core::Result<void> SidecarStore::write(const juce::File& audio, const SampleMetadata& m) const
{
    const juce::String text = toJson(m);
    const juce::File target = jsonFor(audio);
    return services::store::AtomicWriter::writeFileAtomically(target, text.toRawUTF8(), text.getNumBytesAsUTF8());
}
} // namespace services::library
