#include "services/interop/sp16/ValueTreeCodec.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace services::interop::sp16
{
namespace
{
enum Marker : std::uint8_t
{
    kInt = 1,
    kTrue = 2,
    kFalse = 3,
    kDouble = 4,
    kString = 5,
    kInt64 = 6,
    kArray = 7,
    kBinary = 8,
    kUndefined = 9
};

class Reader
{
public:
    Reader(const std::byte* begin, const std::byte* end, const Limits& limits, DecodeInfo& info, const std::byte* base)
        : p_(begin), end_(end), base_(base), limits_(limits), info_(info)
    {
    }

    const std::string& error() const noexcept { return error_; }
    bool atEnd() const noexcept { return p_ == end_; }
    std::size_t left() const noexcept { return static_cast<std::size_t>(end_ - p_); }

    bool fail(std::string_view what)
    {
        if (error_.empty())
            error_ = std::string(what) + " at byte " + std::to_string(p_ - base_);
        return false;
    }

    bool u8(std::uint8_t& v)
    {
        if (p_ == end_)
            return fail("unexpected end of data");
        v = static_cast<std::uint8_t>(*p_++);
        return true;
    }

    bool le(std::uint64_t& v, int n)
    {
        if (left() < static_cast<std::size_t>(n))
            return fail("unexpected end of data");
        v = 0;
        for (int k = 0; k < n; ++k)
            v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(p_[k])) << (8 * k);
        p_ += n;
        return true;
    }

    bool compressed(std::int64_t& v)
    {
        std::uint8_t head = 0;
        if (! u8(head))
            return false;
        const int n = head & 0x7f;
        if (n > 4)
            return fail("compressed int longer than 4 bytes");
        std::uint64_t raw = 0;
        if (! le(raw, n))
            return false;
        const bool negative = (head & 0x80) != 0;
        int needed = 0;
        for (std::uint64_t x = raw; x != 0; x >>= 8)
            ++needed;
        if (n != needed || (negative && raw == 0))
            ++info_.nonMinimalInts;
        v = negative ? -static_cast<std::int64_t>(raw) : static_cast<std::int64_t>(raw);
        return true;
    }

    bool count(std::size_t& out, std::size_t minBytesEach, std::string_view what)
    {
        std::int64_t v = 0;
        if (! compressed(v))
            return false;
        if (v < 0)
            return fail(std::string("negative ") + std::string(what) + " count");
        if (static_cast<std::uint64_t>(v) > limits_.maxChildren)
            return fail(std::string(what) + " count " + std::to_string(v) + " above the limit");
        if (static_cast<std::uint64_t>(v) * minBytesEach > left())
            return fail(std::string(what) + " count " + std::to_string(v) + " larger than the data");
        out = static_cast<std::size_t>(v);
        return true;
    }

    bool cstr(std::string& out)
    {
        const std::size_t n = std::min(left(), limits_.maxString + 1);
        const auto* nul = std::find(p_, p_ + n, std::byte {0});
        if (nul == p_ + n)
            return fail(n > limits_.maxString ? "name longer than the limit" : "unterminated name");
        out.assign(reinterpret_cast<const char*>(p_), static_cast<std::size_t>(nul - p_));
        p_ = nul + 1;
        return true;
    }

    bool var(Var& v, std::size_t depth)
    {
        std::int64_t n = 0;
        if (! compressed(n))
            return false;
        v = Var {};
        if (n == 0)
            return true;
        if (n < 0)
            return fail("negative value size");
        if (static_cast<std::uint64_t>(n) > left())
            return fail("value size larger than the data");
        std::uint8_t marker = 0;
        if (! u8(marker))
            return false;
        const std::size_t payload = static_cast<std::size_t>(n - 1);
        auto exact = [&](std::size_t want) { return payload == want ? true : fail("value size does not fit its type"); };
        std::uint64_t raw = 0;
        switch (marker)
        {
            case kInt:
                if (! exact(4) || ! le(raw, 4))
                    return false;
                v.t = Var::T::Int;
                v.i = static_cast<std::int32_t>(static_cast<std::uint32_t>(raw));
                return true;
            case kTrue:
            case kFalse:
                if (! exact(0))
                    return false;
                v.t = marker == kTrue ? Var::T::True : Var::T::False;
                return true;
            case kDouble:
                if (! exact(8) || ! le(raw, 8))
                    return false;
                v.t = Var::T::Double;
                std::memcpy(&v.d, &raw, sizeof v.d);
                return true;
            case kInt64:
                if (! exact(8) || ! le(raw, 8))
                    return false;
                v.t = Var::T::Int64;
                v.i = static_cast<std::int64_t>(raw);
                return true;
            case kUndefined:
                if (! exact(0))
                    return false;
                v.t = Var::T::Undefined;
                return true;
            case kString:
                if (payload > limits_.maxString + 1)
                    return fail("string longer than the limit");
                v.t = Var::T::String;
                if (payload > 0 && p_[payload - 1] == std::byte {0})
                    v.s.assign(reinterpret_cast<const char*>(p_), payload - 1);
                else
                {
                    v.s.assign(reinterpret_cast<const char*>(p_), payload);
                    v.unterminated = true;
                }
                p_ += payload;
                return true;
            case kBinary:
                v.t = Var::T::Binary;
                v.bin.assign(p_, p_ + payload);
                p_ += payload;
                return true;
            case kArray:
            {
                if (depth + 1 > limits_.maxDepth)
                    return fail("arrays nested deeper than the limit");
                info_.maxDepth = std::max(info_.maxDepth, depth + 1);
                Reader sub(p_, p_ + payload, limits_, info_, base_);
                std::size_t items = 0;
                if (! sub.count(items, 1, "array"))
                    return adopt(sub);
                v.t = Var::T::Array;
                v.a.resize(items);
                for (auto& e : v.a)
                    if (! sub.var(e, depth + 1))
                        return adopt(sub);
                if (! sub.atEnd())
                    return adopt(sub, "array size does not match its elements");
                p_ += payload;
                return true;
            }
            default: break;
        }
        return fail("unknown value type marker " + std::to_string(marker));
    }

    bool node(Node& out, std::size_t depth)
    {
        if (depth > limits_.maxDepth)
            return fail("nodes nested deeper than the limit");
        info_.maxDepth = std::max(info_.maxDepth, depth);
        ++info_.nodes;
        if (! cstr(out.type))
            return false;
        std::size_t props = 0;
        if (! count(props, 2, "property")) // a name (≥ 1 byte) and a var (≥ 1 byte)
            return false;
        out.props.resize(props);
        info_.props += props;
        for (auto& [name, value] : out.props)
            if (! cstr(name) || ! var(value, depth))
                return false;
        std::size_t children = 0;
        if (! count(children, 3, "child")) // type NUL + two counts
            return false;
        out.children.resize(children);
        for (auto& c : out.children)
            if (! node(c, depth + 1))
                return false;
        return true;
    }

private:
    bool adopt(const Reader& sub, std::string_view what = {})
    {
        if (error_.empty())
            error_ = what.empty() ? sub.error_ : std::string(what) + " at byte " + std::to_string(sub.p_ - base_);
        return false;
    }

    const std::byte* p_;
    const std::byte* end_;
    const std::byte* base_;
    const Limits& limits_;
    DecodeInfo& info_;
    std::string error_;
};

class Writer
{
public:
    std::vector<std::byte> out;

    void byte(std::uint8_t b) { out.push_back(static_cast<std::byte>(b)); }
    void le(std::uint64_t v, int n)
    {
        for (int k = 0; k < n; ++k)
            byte(static_cast<std::uint8_t>(v >> (8 * k)));
    }
    void compressed(std::int64_t v)
    {
        const std::uint64_t mag = v < 0 ? static_cast<std::uint64_t>(-v) : static_cast<std::uint64_t>(v);
        int n = 0;
        for (std::uint64_t x = mag; x != 0; x >>= 8)
            ++n;
        byte(static_cast<std::uint8_t>(n | (v < 0 ? 0x80 : 0)));
        le(mag, n);
    }
    void cstr(std::string_view s)
    {
        for (const char c : s)
            byte(static_cast<std::uint8_t>(c));
        byte(0);
    }
    void var(const Var& v)
    {
        switch (v.t)
        {
            case Var::T::Void: compressed(0); return;
            case Var::T::Int: compressed(5); byte(kInt); le(static_cast<std::uint32_t>(v.i), 4); return;
            case Var::T::True: compressed(1); byte(kTrue); return;
            case Var::T::False: compressed(1); byte(kFalse); return;
            case Var::T::Undefined: compressed(1); byte(kUndefined); return;
            case Var::T::Double:
            {
                std::uint64_t raw = 0;
                std::memcpy(&raw, &v.d, sizeof raw);
                compressed(9);
                byte(kDouble);
                le(raw, 8);
                return;
            }
            case Var::T::Int64: compressed(9); byte(kInt64); le(static_cast<std::uint64_t>(v.i), 8); return;
            case Var::T::String:
                compressed(static_cast<std::int64_t>(v.s.size()) + (v.unterminated ? 1 : 2));
                byte(kString);
                for (const char c : v.s)
                    byte(static_cast<std::uint8_t>(c));
                if (! v.unterminated)
                    byte(0);
                return;
            case Var::T::Binary:
                compressed(static_cast<std::int64_t>(v.bin.size()) + 1);
                byte(kBinary);
                out.insert(out.end(), v.bin.begin(), v.bin.end());
                return;
            case Var::T::Array:
            {
                Writer body;
                body.compressed(static_cast<std::int64_t>(v.a.size()));
                for (const auto& e : v.a)
                    body.var(e);
                compressed(static_cast<std::int64_t>(body.out.size()) + 1);
                byte(kArray);
                out.insert(out.end(), body.out.begin(), body.out.end());
                return;
            }
        }
    }
    void node(const Node& n)
    {
        cstr(n.type);
        compressed(static_cast<std::int64_t>(n.props.size()));
        for (const auto& [name, value] : n.props)
        {
            cstr(name);
            var(value);
        }
        compressed(static_cast<std::int64_t>(n.children.size()));
        for (const auto& c : n.children)
            node(c);
    }
};

void jsonString(std::string& out, std::string_view s)
{
    out += '"';
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += ch;
        }
        else if (c < 0x20)
        {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        }
        else
            out += ch;
    }
    out += '"';
}

void varJson(std::string& out, const Var& v)
{
    char buf[40];
    switch (v.t)
    {
        case Var::T::Void: out += "{\"void\":null}"; return;
        case Var::T::Undefined: out += "{\"undefined\":null}"; return;
        case Var::T::Int: out += "{\"int\":" + std::to_string(v.i) + "}"; return;
        case Var::T::Int64: out += "{\"int64\":\"" + std::to_string(v.i) + "\"}"; return;
        case Var::T::True: out += "{\"bool\":true}"; return;
        case Var::T::False: out += "{\"bool\":false}"; return;
        case Var::T::Double:
            std::snprintf(buf, sizeof buf, "%.17g", v.d);
            out += "{\"double\":";
            out += (std::isfinite(v.d) ? std::string(buf) : "\"" + std::string(buf) + "\"");
            out += "}";
            return;
        case Var::T::String:
            out += v.unterminated ? "{\"rawString\":" : "{\"string\":";
            jsonString(out, v.s);
            out += "}";
            return;
        case Var::T::Binary:
            out += "{\"binary\":\"" + toBase64(v.bin) + "\"}";
            return;
        case Var::T::Array:
            out += "{\"array\":[";
            for (std::size_t k = 0; k < v.a.size(); ++k)
            {
                if (k)
                    out += ',';
                varJson(out, v.a[k]);
            }
            out += "]}";
            return;
    }
}
} // namespace

std::string_view typeName(Var::T t) noexcept
{
    switch (t)
    {
        case Var::T::Void: return "void";
        case Var::T::Int: return "int";
        case Var::T::True:
        case Var::T::False: return "bool";
        case Var::T::Double: return "double";
        case Var::T::String: return "string";
        case Var::T::Int64: return "int64";
        case Var::T::Array: return "array";
        case Var::T::Binary: return "binary";
        case Var::T::Undefined: return "undefined";
    }
    return "?";
}

const Var* Node::prop(std::string_view name) const noexcept
{
    for (const auto& [n, v] : props)
        if (n == name)
            return &v;
    return nullptr;
}

std::pair<std::string_view, std::optional<int>> splitSuffix(std::string_view type) noexcept
{
    std::size_t k = type.size();
    while (k > 0 && type[k - 1] >= '0' && type[k - 1] <= '9')
        --k;
    if (k == type.size() || k == 0 || type.size() - k > 9)
        return {type, std::nullopt};
    int v = 0;
    for (std::size_t j = k; j < type.size(); ++j)
        v = v * 10 + (type[j] - '0');
    return {type.substr(0, k), v};
}

Result<Node> decodeTree(std::span<const std::byte> bytes, const Limits& limits, DecodeInfo* info)
{
    DecodeInfo local;
    DecodeInfo& in = info != nullptr ? *info : local;
    in = {};
    Reader r(bytes.data(), bytes.data() + bytes.size(), limits, in, bytes.data());
    Node root;
    if (! r.node(root, 0))
        return Result<Node>::failure("The project tree is damaged (" + r.error() + ")");
    if (! r.atEnd())
        return Result<Node>::failure("The project tree is damaged (" + std::to_string(r.left()) +
                                     " bytes after the root node)");
    return root;
}

std::vector<std::byte> encodeTree(const Node& n)
{
    Writer w;
    w.node(n);
    return std::move(w.out);
}

bool hasContainerMagic(std::span<const std::byte> bytes) noexcept
{
    return bytes.size() >= kContainerMagic.size() &&
           std::memcmp(bytes.data(), kContainerMagic.data(), kContainerMagic.size()) == 0;
}

Result<Node> readContainer(std::span<const std::byte> bytes, const Limits& limits, DecodeInfo* info,
                           std::size_t* inflatedSize)
{
    if (! hasContainerMagic(bytes))
        return Result<Node>::failure("Not a reference project file (no vltrgzip header)");
    auto inflated = inflateZlib(bytes.subspan(kContainerMagic.size()), limits.maxInflated);
    if (! inflated)
        return inflated.error();
    if (inflatedSize != nullptr)
        *inflatedSize = inflated.value().size();
    return decodeTree(inflated.value(), limits, info);
}

std::vector<std::byte> writeContainer(const Node& n)
{
    const auto tree = encodeTree(n);
    const auto z = deflateZlib(tree, kOuterLevel);
    const auto magic = asBytes(kContainerMagic);
    // One sized buffer and two copies (vector::insert of a possibly empty range made GCC 11 report a read from a
    // zero-sized region, -Wstringop-overread, in the device build).
    std::vector<std::byte> out(magic.size() + z.size());
    std::memcpy(out.data(), magic.data(), magic.size());
    if (! z.empty())
        std::memcpy(out.data() + magic.size(), z.data(), z.size());
    return out;
}

std::uint32_t adler32(std::span<const std::byte> data) noexcept
{
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1, b = 0;
    std::size_t k = 0;
    while (k < data.size())
    {
        const std::size_t n = std::min<std::size_t>(5552, data.size() - k); // no overflow before the modulo
        for (std::size_t j = 0; j < n; ++j)
        {
            a += static_cast<std::uint8_t>(data[k + j]);
            b += a;
        }
        a %= kMod;
        b %= kMod;
        k += n;
    }
    return (b << 16) | a;
}

Result<std::vector<std::byte>> inflateZlib(std::span<const std::byte> in, std::size_t maxOut)
{
    using R = Result<std::vector<std::byte>>;
    if (in.size() < 6)
        return R::failure("The compressed data is too short");
    const auto cmf = static_cast<std::uint8_t>(in[0]), flg = static_cast<std::uint8_t>(in[1]);
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0)
        return R::failure("The compressed data has no valid zlib header");
    juce::MemoryInputStream src(in.data(), in.size(), false);
    juce::GZIPDecompressorInputStream z(&src, false, juce::GZIPDecompressorInputStream::zlibFormat);
    std::vector<std::byte> out;
    out.reserve(std::min(maxOut, in.size() * 8));
    std::vector<std::byte> chunk(1 << 16);
    for (;;)
    {
        const int n = z.read(chunk.data(), static_cast<int>(chunk.size()));
        if (n <= 0)
            break;
        if (out.size() + static_cast<std::size_t>(n) > maxOut)
            return R::failure("The compressed data expands beyond " + std::to_string(maxOut) + " bytes");
        out.insert(out.end(), chunk.begin(), chunk.begin() + n);
    }
    std::uint32_t stored = 0;
    for (std::size_t k = in.size() - 4; k < in.size(); ++k)
        stored = (stored << 8) | static_cast<std::uint8_t>(in[k]);
    if (stored != adler32(out))
        return R::failure("The compressed data is damaged or truncated (checksum mismatch)");
    return out;
}

std::vector<std::byte> deflateZlib(std::span<const std::byte> in, int level)
{
    juce::MemoryOutputStream mo;
    {
        juce::GZIPCompressorOutputStream z(mo, level, 0); // windowBits 0 = 15, zlib header
        z.write(in.data(), in.size());
    } // the destructor finishes the stream
    const auto* d = static_cast<const std::byte*>(mo.getData());
    return std::vector<std::byte>(d, d + mo.getDataSize());
}

std::span<const std::byte> asBytes(std::string_view s) noexcept
{
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string toBase64(std::span<const std::byte> data)
{
    return juce::Base64::toBase64(data.data(), data.size()).toStdString();
}

std::optional<std::vector<std::byte>> fromBase64(std::string_view text)
{
    juce::MemoryOutputStream mo;
    if (! juce::Base64::convertFromBase64(mo, juce::String::fromUTF8(text.data(), static_cast<int>(text.size()))))
        return std::nullopt;
    const auto* d = static_cast<const std::byte*>(mo.getData());
    return std::vector<std::byte>(d, d + mo.getDataSize());
}

std::string varToJson(const Var& v)
{
    std::string out;
    varJson(out, v);
    return out;
}
} // namespace services::interop::sp16
