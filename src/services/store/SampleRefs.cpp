#include "services/store/SampleRefs.h"

#include <cstring>

namespace services::store
{
namespace
{
constexpr std::array<std::uint32_t, 64> kRound {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

constexpr std::string_view kLib = "$Lib", kBundle = "$Bundle", kExt = "$Ext:", kAbsoluteTag = "absolute";

// A relative path is safe when it is non-empty and every '/'-separated component is a plain name.
bool safeRelative(std::string_view rel) noexcept
{
    if (rel.empty() || rel.front() == '/')
        return false;
    std::size_t start = 0;
    while (start <= rel.size())
    {
        const auto end = rel.find('/', start);
        const auto part = rel.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string_view::npos ||
            part.find('\0') != std::string_view::npos)
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

juce::File child(const juce::File& root, std::string_view rel)
{
    return root.getChildFile(juce::String::fromUTF8(rel.data(), static_cast<int>(rel.size())));
}
} // namespace

void Sha256::reset() noexcept
{
    h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    length_ = 0;
    used_ = 0;
}

void Sha256::block(const std::uint8_t* p) noexcept
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i)
    {
        const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i)
    {
        const auto t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept
{
    auto p = static_cast<const std::uint8_t*>(data);
    length_ += size;
    if (used_ > 0)
    {
        const auto n = std::min(size, buffer_.size() - used_);
        std::memcpy(buffer_.data() + used_, p, n);
        used_ += n, p += n, size -= n;
        if (used_ < buffer_.size())
            return;
        block(buffer_.data());
        used_ = 0;
    }
    for (; size >= 64; p += 64, size -= 64)
        block(p);
    std::memcpy(buffer_.data(), p, size);
    used_ = size;
}

std::array<std::uint8_t, 32> Sha256::finish() noexcept
{
    const auto bits = length_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (used_ != 56)
        update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    std::array<std::uint8_t, 32> out {};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            out[static_cast<std::size_t>(4 * i + j)] = static_cast<std::uint8_t>(h_[static_cast<std::size_t>(i)] >> (24 - 8 * j));
    reset();
    return out;
}

std::string Sha256::hex(const std::array<std::uint8_t, 32>& digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        s[2 * i] = digits[digest[i] >> 4];
        s[2 * i + 1] = digits[digest[i] & 15];
    }
    return s;
}

std::string contentIdOf(const void* data, std::size_t size)
{
    Sha256 sha;
    sha.update(data, size);
    return std::string(kContentIdPrefix) + Sha256::hex(sha.finish());
}

core::Result<std::string> contentId(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (! in.openedOk())
        return core::Result<std::string>::failure("Cannot read " + file.getFullPathName().toStdString(),
                                                  in.getStatus().getErrorMessage().toStdString());
    Sha256 sha;
    std::vector<char> buffer(64 * 1024);
    for (;;)
    {
        const auto n = in.read(buffer.data(), static_cast<int>(buffer.size()));
        if (n < 0)
            return core::Result<std::string>::failure("Cannot read " + file.getFullPathName().toStdString());
        if (n == 0)
            break;
        sha.update(buffer.data(), static_cast<std::size_t>(n));
    }
    return std::string(kContentIdPrefix) + Sha256::hex(sha.finish());
}

bool isContentId(std::string_view id) noexcept
{
    if (id.size() != kContentIdPrefix.size() + 64 || id.substr(0, kContentIdPrefix.size()) != kContentIdPrefix)
        return false;
    for (const char c : id.substr(kContentIdPrefix.size()))
        if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

SampleLocation splitLocation(std::string_view s)
{
    const auto prefixed = [&](std::string_view prefix, LocationKind kind) -> std::optional<SampleLocation>
    {
        if (s.size() <= prefix.size() + 1 || s.substr(0, prefix.size()) != prefix || s[prefix.size()] != '/')
            return std::nullopt;
        return SampleLocation {kind, {}, std::string(s.substr(prefix.size() + 1))};
    };
    if (auto lib = prefixed(kLib, LocationKind::Library))
        return *lib;
    if (auto bundle = prefixed(kBundle, LocationKind::Bundle))
        return *bundle;
    if (s.substr(0, kExt.size()) == kExt)
    {
        const auto slash = s.find('/', kExt.size());
        if (slash != std::string_view::npos && slash > kExt.size() && slash + 1 < s.size())
            return {LocationKind::External, std::string(s.substr(kExt.size(), slash - kExt.size())),
                    std::string(s.substr(slash + 1))};
    }
    return {LocationKind::Absolute, {}, std::string(s)};
}

std::string joinLocation(const SampleLocation& l)
{
    switch (l.kind)
    {
        case LocationKind::Library: return std::string(kLib) + "/" + l.path;
        case LocationKind::Bundle: return std::string(kBundle) + "/" + l.path;
        case LocationKind::External: return std::string(kExt) + l.volume + "/" + l.path;
        case LocationKind::Absolute: break;
    }
    return l.path;
}

std::string locationTag(const SampleLocation& l)
{
    switch (l.kind)
    {
        case LocationKind::Library: return std::string(kLib);
        case LocationKind::Bundle: return std::string(kBundle);
        case LocationKind::External: return std::string(kExt) + l.volume;
        case LocationKind::Absolute: break;
    }
    return std::string(kAbsoluteTag);
}

std::optional<SampleLocation> parseLocationTag(std::string_view tag, std::string path)
{
    SampleLocation l;
    if (tag == kLib)
        l.kind = LocationKind::Library;
    else if (tag == kBundle)
        l.kind = LocationKind::Bundle;
    else if (tag.size() > kExt.size() && tag.substr(0, kExt.size()) == kExt)
    {
        l.kind = LocationKind::External;
        l.volume = std::string(tag.substr(kExt.size()));
    }
    else if (tag == kAbsoluteTag)
        l.kind = LocationKind::Absolute;
    else
        return std::nullopt;
    l.path = std::move(path);
    return l;
}

std::optional<juce::File> resolveLocation(const SampleLocation& l, const SampleRoots& roots)
{
    switch (l.kind)
    {
        case LocationKind::Library:
            if (roots.library == juce::File() || ! safeRelative(l.path))
                return std::nullopt;
            return child(roots.library, l.path);
        case LocationKind::Bundle:
            if (roots.bundle == juce::File() || ! safeRelative(l.path))
                return std::nullopt;
            return child(roots.bundle.getChildFile("samples"), l.path);
        case LocationKind::External:
        {
            const auto it = roots.externals.find(l.volume);
            if (it == roots.externals.end() || ! safeRelative(l.path))
                return std::nullopt;
            return child(it->second, l.path);
        }
        case LocationKind::Absolute:
            if (! juce::File::isAbsolutePath(juce::String::fromUTF8(l.path.c_str())))
                return std::nullopt;
            return juce::File(juce::String::fromUTF8(l.path.c_str()));
    }
    return std::nullopt;
}

int resolvePool(core::model::SamplePool& pool, const SampleRoots& roots)
{
    int missing = 0;
    for (auto& entry : pool.entries)
    {
        const auto file = resolveLocation(splitLocation(entry.path), roots);
        entry.missing = ! file || ! file->existsAsFile();
        missing += entry.missing ? 1 : 0;
    }
    return missing;
}

std::string safeFileName(std::string_view name)
{
    std::string s;
    s.reserve(name.size());
    for (const char ch : name)
    {
        const auto c = static_cast<unsigned char>(ch);
        const bool bad = c < 0x20 || c == 0x7f || std::strchr("<>:\"/\\|?*", ch) != nullptr;
        s.push_back(bad ? '_' : ch);
    }
    while (! s.empty() && (s.front() == ' ' || s.front() == '.'))
        s.erase(s.begin());
    while (! s.empty() && (s.back() == ' ' || s.back() == '.'))
        s.pop_back();
    constexpr std::size_t kMax = 64;
    if (s.size() > kMax)
    {
        const auto dot = s.rfind('.');
        std::string ext = dot != std::string::npos && s.size() - dot <= 8 ? s.substr(dot) : std::string();
        auto keep = kMax - ext.size();
        while (keep > 0 && (static_cast<unsigned char>(s[keep]) & 0xC0) == 0x80) // never split a UTF-8 sequence
            --keep;
        s = s.substr(0, keep) + ext;
    }
    return s.empty() ? std::string("sample") : s;
}
} // namespace services::store
