// XXH64 (the xxHash 64-bit algorithm, Yann Collet's public specification; this is an independent implementation, no
// code copied), streaming. The sample pool's key (SampleBuffer::contentId) is the XXH64 of a file's PCM bytes with a
// seed made from the format (channels, rate, encoding), computed while decoding.
// Budget: ≥ 200 MB/s on one A9 core (bench_decode `decode_hash`); worker threads only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace services::library
{
class Xxh64
{
public:
    explicit Xxh64(std::uint64_t seed = 0) noexcept { reset(seed); }
    void reset(std::uint64_t seed = 0) noexcept;
    void update(const void* data, std::size_t size) noexcept;
    std::uint64_t digest() const noexcept; // the state is kept: more update() calls may follow

    static std::uint64_t of(const void* data, std::size_t size, std::uint64_t seed = 0) noexcept;

private:
    std::uint64_t v_[4]{};
    std::uint64_t seed_ = 0, total_ = 0;
    unsigned char buffer_[32]{};
    std::size_t used_ = 0;
};
} // namespace services::library
