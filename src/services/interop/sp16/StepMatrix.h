// The reference's binary blobs inside the project tree (spec sp16-interop.md "Step matrix", "Arrangement"; research
// docs/project-format.md "PatternData.Steps — the step matrix (decoded)" and "ArrangementData.Sections").
//
//   PatternData.Steps        zlib (level 6, 78 9c) → 155,648 bytes = 16 tracks × 64 steps × 152-byte records,
//                            TRACK-major (offset = track × 9728 + step × 152; record index track × 64 + step). Record:
//                            int32 track · int32 step (a programmed record names its own position; empty −127 / −126) ·
//                            int32 type/flags (15 in every record seen; 11 / 8 kept from the spec for HALF / PARAM) ·
//                            35 × uint32 lock words [vct:2 | value:22 signed | idx:8]; no lock = 0x3f800000 | idx (the
//                            packed −32768 sentinel). All little-endian.
//                            The layout was established from two real 1.63 projects (versionNo 3, minorVersionNo 1): all
//                            2,759 programmed records sit at track × 64 + step and hold exactly that (track, step), and
//                            1908 of 1909 fall inside their pattern's Length. The research page reads the matrix
//                            step-major with a "trigger" and a "value2" field; that reading scrambles every pattern.
//   ArrangementData.Sections zlib (level 6) → 7,992 bytes = 999 × 8-byte records (two int32; default 65536, 0; field
//                            meanings unknown, OQ-02).
//
// Pure functions, bounded (the inflated size is checked against the fixed layout). Worker threads only.
#pragma once

#include "services/interop/sp16/ValueTreeCodec.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace services::interop::sp16
{
constexpr int kStepCount = 64, kTrackCount = 16, kLockWords = 35;
constexpr std::size_t kRecordBytes = 12 + 4 * kLockWords;                         // 152
constexpr std::size_t kStepsBytes = kStepCount * kTrackCount * kRecordBytes;       // 155,648
constexpr std::int32_t kEmptyTrigger = -127, kEmptyValue2 = -126, kEmptyTypeFlags = 15;

// The file position of the record of (step, track), both 0-based: the matrix is track-major.
constexpr std::size_t recordIndex(int step, int track) noexcept
{
    return static_cast<std::size_t>(track) * kStepCount + static_cast<std::size_t>(step);
}
constexpr int stepOfRecord(std::size_t index) noexcept { return static_cast<int>(index % kStepCount); }
constexpr int trackOfRecord(std::size_t index) noexcept { return static_cast<int>(index / kStepCount); }
constexpr std::int32_t kNoLockValue = -32768;     // the 22-bit "no lock" sentinel
constexpr std::int32_t kValueFull = 2097151;      // 2^21 − 1: continuous value = value / kValueFull
constexpr std::int32_t kK127 = 16513;             // kValueFull / 127: one step of a 0–127 value
constexpr std::int32_t kFrameBase = 1411200;      // ordinals 1–3: low part base (44,100 × 32)
constexpr int kSectionCount = 999;
constexpr std::size_t kSectionsBytes = kSectionCount * 8; // 7,992

// The index byte of an empty ("no lock") word: the slot number for slots 0–22; for the FX slots observed 5 + k
// (sub-slot k 0–5 of FX1 23–28 and FX2 29–34) in a real project.
constexpr std::uint8_t noneIndex(int slot) noexcept
{
    return static_cast<std::uint8_t>(slot < 23 ? slot : 5 + (slot - 23) % 6);
}

struct LockWord
{
    std::uint8_t vct = 0;      // value-change type (2 bits; meaning unknown)
    std::int32_t value22 = kNoLockValue;
    std::uint8_t idx = 0;      // low byte (the slot's parameter index)
    bool empty = true;         // vct == 0 && value22 == −32768

    static LockWord unpack(std::uint32_t w) noexcept;
    std::uint32_t pack() const noexcept;
    static LockWord none(std::uint8_t idx) noexcept { return unpack(0x3f800000u | idx); }
    friend bool operator== (const LockWord&, const LockWord&) = default;
};

struct StepRecord
{
    // `trigger` / `value2`: the spec's names for the first two fields. A programmed record holds its own track (0–15)
    // and step (0–63) there; an empty one −127 / −126.
    std::int32_t trigger = kEmptyTrigger, value2 = kEmptyValue2, typeFlags = kEmptyTypeFlags;
    std::array<LockWord, kLockWords> slots {};

    static StepRecord emptyRecord() noexcept; // trigger −127, value2 −126, type 15, slot s = none(noneIndex(s))
    bool hasLocks() const noexcept;
    bool programmed() const noexcept { return trigger != kEmptyTrigger; }
    // A programmed record whose first two fields name the position it is stored at.
    bool namesPosition(int step, int track) const noexcept { return trigger == track && value2 == step; }
    friend bool operator== (const StepRecord&, const StepRecord&) = default;
};

// 1024 records in file order (index = recordIndex(step, track) = track × 64 + step).
Result<std::vector<StepRecord>> decodeSteps(std::span<const std::byte> zlibBlob);
Result<std::vector<StepRecord>> decodeStepsRaw(std::span<const std::byte> inflated); // the 155,648 bytes
std::vector<std::byte> encodeStepsRaw(std::span<const StepRecord>);                  // 155,648 bytes
std::vector<std::byte> encodeSteps(std::span<const StepRecord>);                     // zlib level 6

struct SectionRecord
{
    std::int32_t a = 65536, b = 0; // unknown fields (OQ-02); the default record
    bool isDefault() const noexcept { return a == 65536 && b == 0; }
    friend bool operator== (const SectionRecord&, const SectionRecord&) = default;
};
Result<std::vector<SectionRecord>> decodeSections(std::span<const std::byte> zlibBlob); // any multiple of 8 bytes
std::vector<std::byte> encodeSections(std::span<const SectionRecord>);                  // zlib level 6
} // namespace services::interop::sp16
