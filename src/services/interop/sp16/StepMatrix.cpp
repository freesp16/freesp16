#include "services/interop/sp16/StepMatrix.h"

#include <algorithm>
#include <string>

namespace services::interop::sp16
{
namespace
{
std::uint32_t rd32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24;
}
void wr32(std::byte* p, std::uint32_t v) noexcept
{
    for (int k = 0; k < 4; ++k)
        p[k] = static_cast<std::byte>(v >> (8 * k));
}
} // namespace

LockWord LockWord::unpack(std::uint32_t w) noexcept
{
    LockWord l;
    l.idx = static_cast<std::uint8_t>(w & 0xffu);
    const std::uint32_t v = (w >> 8) & 0x3fffffu;
    l.value22 = (v & 0x200000u) != 0 ? static_cast<std::int32_t>(v) - 0x400000 : static_cast<std::int32_t>(v);
    l.vct = static_cast<std::uint8_t>(w >> 30);
    l.empty = l.vct == 0 && l.value22 == kNoLockValue;
    return l;
}

std::uint32_t LockWord::pack() const noexcept
{
    return static_cast<std::uint32_t>(vct & 3u) << 30 | (static_cast<std::uint32_t>(value22) & 0x3fffffu) << 8 | idx;
}

StepRecord StepRecord::emptyRecord() noexcept
{
    StepRecord r;
    for (int s = 0; s < kLockWords; ++s)
        r.slots[static_cast<std::size_t>(s)] = LockWord::none(noneIndex(s));
    return r;
}

bool StepRecord::hasLocks() const noexcept
{
    for (const auto& l : slots)
        if (! l.empty)
            return true;
    return false;
}

Result<std::vector<StepRecord>> decodeStepsRaw(std::span<const std::byte> in)
{
    using R = Result<std::vector<StepRecord>>;
    if (in.size() != kStepsBytes)
        return R::failure("The step matrix has " + std::to_string(in.size()) + " bytes, expected " +
                          std::to_string(kStepsBytes));
    std::vector<StepRecord> out(static_cast<std::size_t>(kStepCount * kTrackCount));
    for (std::size_t k = 0; k < out.size(); ++k)
    {
        const std::byte* p = in.data() + k * kRecordBytes;
        auto& r = out[k];
        r.trigger = static_cast<std::int32_t>(rd32(p));
        r.value2 = static_cast<std::int32_t>(rd32(p + 4));
        r.typeFlags = static_cast<std::int32_t>(rd32(p + 8));
        for (int s = 0; s < kLockWords; ++s)
            r.slots[static_cast<std::size_t>(s)] = LockWord::unpack(rd32(p + 12 + 4 * s));
    }
    return out;
}

Result<std::vector<StepRecord>> decodeSteps(std::span<const std::byte> blob)
{
    auto raw = inflateZlib(blob, kStepsBytes);
    if (! raw)
        return raw.error();
    return decodeStepsRaw(raw.value());
}

std::vector<std::byte> encodeStepsRaw(std::span<const StepRecord> records)
{
    std::vector<std::byte> out(kStepsBytes);
    const std::size_t n = std::min<std::size_t>(records.size(), static_cast<std::size_t>(kStepCount * kTrackCount));
    const auto empty = StepRecord::emptyRecord();
    for (std::size_t k = 0; k < static_cast<std::size_t>(kStepCount * kTrackCount); ++k)
    {
        const auto& r = k < n ? records[k] : empty;
        std::byte* p = out.data() + k * kRecordBytes;
        wr32(p, static_cast<std::uint32_t>(r.trigger));
        wr32(p + 4, static_cast<std::uint32_t>(r.value2));
        wr32(p + 8, static_cast<std::uint32_t>(r.typeFlags));
        for (int s = 0; s < kLockWords; ++s)
            wr32(p + 12 + 4 * s, r.slots[static_cast<std::size_t>(s)].pack());
    }
    return out;
}

std::vector<std::byte> encodeSteps(std::span<const StepRecord> records)
{
    return deflateZlib(encodeStepsRaw(records), kInnerLevel);
}

Result<std::vector<SectionRecord>> decodeSections(std::span<const std::byte> blob)
{
    using R = Result<std::vector<SectionRecord>>;
    auto raw = inflateZlib(blob, kSectionsBytes * 4);
    if (! raw)
        return raw.error();
    const auto& b = raw.value();
    if (b.size() % 8 != 0)
        return R::failure("The arrangement blob has " + std::to_string(b.size()) + " bytes, not a multiple of 8");
    std::vector<SectionRecord> out(b.size() / 8);
    for (std::size_t k = 0; k < out.size(); ++k)
    {
        out[k].a = static_cast<std::int32_t>(rd32(b.data() + 8 * k));
        out[k].b = static_cast<std::int32_t>(rd32(b.data() + 8 * k + 4));
    }
    return out;
}

std::vector<std::byte> encodeSections(std::span<const SectionRecord> records)
{
    std::vector<std::byte> raw(records.size() * 8);
    for (std::size_t k = 0; k < records.size(); ++k)
    {
        wr32(raw.data() + 8 * k, static_cast<std::uint32_t>(records[k].a));
        wr32(raw.data() + 8 * k + 4, static_cast<std::uint32_t>(records[k].b));
    }
    return deflateZlib(raw, kInnerLevel);
}
} // namespace services::interop::sp16
