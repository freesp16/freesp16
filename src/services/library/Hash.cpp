#include "services/library/Hash.h"

#include <cstring>

namespace services::library
{
namespace
{
constexpr std::uint64_t P1 = 11400714785074694791ULL;
constexpr std::uint64_t P2 = 14029467366897019727ULL;
constexpr std::uint64_t P3 = 1609587929392839161ULL;
constexpr std::uint64_t P4 = 9650029242287828579ULL;
constexpr std::uint64_t P5 = 2870177450012600261ULL;

inline std::uint64_t rotl(std::uint64_t x, int r) noexcept
{
    return (x << r) | (x >> (64 - r));
}
inline std::uint64_t read64(const unsigned char* p) noexcept
{
    std::uint64_t v; // little-endian hosts and device (armel); memcpy compiles to unaligned loads
    std::memcpy(&v, p, 8);
    return v;
}
inline std::uint32_t read32(const unsigned char* p) noexcept
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline std::uint64_t round(std::uint64_t acc, std::uint64_t input) noexcept
{
    acc += input * P2;
    acc = rotl(acc, 31);
    return acc * P1;
}
inline std::uint64_t mergeRound(std::uint64_t acc, std::uint64_t v) noexcept
{
    acc ^= round(0, v);
    return acc * P1 + P4;
}
// Consumes whole 32-byte stripes from p; returns the bytes consumed.
inline std::size_t stripes(std::uint64_t* v, const unsigned char* p, std::size_t size) noexcept
{
    std::uint64_t a = v[0], b = v[1], c = v[2], d = v[3];
    std::size_t done = 0;
    for (; done + 32 <= size; done += 32)
    {
        a = round(a, read64(p + done));
        b = round(b, read64(p + done + 8));
        c = round(c, read64(p + done + 16));
        d = round(d, read64(p + done + 24));
    }
    v[0] = a, v[1] = b, v[2] = c, v[3] = d;
    return done;
}
} // namespace

void Xxh64::reset(std::uint64_t seed) noexcept
{
    seed_ = seed;
    v_[0] = seed + P1 + P2;
    v_[1] = seed + P2;
    v_[2] = seed;
    v_[3] = seed - P1;
    total_ = 0;
    used_ = 0;
}

void Xxh64::update(const void* data, std::size_t size) noexcept
{
    const auto* p = static_cast<const unsigned char*>(data);
    total_ += size;
    if (used_ > 0)
    {
        const std::size_t take = size < 32 - used_ ? size : 32 - used_;
        std::memcpy(buffer_ + used_, p, take);
        used_ += take;
        p += take;
        size -= take;
        if (used_ < 32)
            return;
        stripes(v_, buffer_, 32);
        used_ = 0;
    }
    const std::size_t done = stripes(v_, p, size);
    p += done;
    size -= done;
    if (size > 0)
    {
        std::memcpy(buffer_, p, size);
        used_ = size;
    }
}

std::uint64_t Xxh64::digest() const noexcept
{
    std::uint64_t h;
    if (total_ >= 32)
    {
        h = rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18);
        h = mergeRound(h, v_[0]);
        h = mergeRound(h, v_[1]);
        h = mergeRound(h, v_[2]);
        h = mergeRound(h, v_[3]);
    }
    else
        h = seed_ + P5;
    h += total_;
    const unsigned char* p = buffer_;
    std::size_t n = used_;
    for (; n >= 8; n -= 8, p += 8)
    {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
    }
    if (n >= 4)
    {
        h ^= static_cast<std::uint64_t>(read32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
        n -= 4;
    }
    for (; n > 0; --n, ++p)
    {
        h ^= static_cast<std::uint64_t>(*p) * P5;
        h = rotl(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

std::uint64_t Xxh64::of(const void* data, std::size_t size, std::uint64_t seed) noexcept
{
    Xxh64 x(seed);
    x.update(data, size);
    return x.digest();
}
} // namespace services::library
