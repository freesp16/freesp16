// Sample-region helpers shared by the engine (engine/voice), the UI (PLAYBACK page) and the importer. Pure integer
// functions, no allocation, constexpr where possible.
//
//   clampRegion   PLY-001 invariants: 0 ≤ start < end ≤ frames, start ≤ loopStart < end, end = start + length
//   coarseStep    PLY-002: one encoder detent = frames / 128 (at least 1 frame); SHIFT = 1 frame
//   ccToFrame     PLY-003: CC 21–23, 0–127 linear over the whole sample
//   grid*         PLY-004: GRID SNAP — START / LOOP START as grid positions 1…N of the slice grid (SET SLICE,
//                 SliceSettings::gridCount, default 16, at most 64 — OQ-21), position n at frame (n − 1)·frames / N
#pragma once

#include <cstdint>

namespace core::playback
{
constexpr int kDefaultGridCount = 16;
constexpr int kMaxGridCount = 64;
constexpr int kCoarseDetents = 128;
// "The whole sample": the default length before a sample is known (clampRegion() shortens it to the sample).
constexpr std::int64_t kWholeSample = INT64_MAX / 4;

struct RegionFrames
{
    std::int64_t start = 0, loopStart = 0, length = kWholeSample;
    constexpr std::int64_t end() const noexcept { return start + length; }
    friend constexpr bool operator== (const RegionFrames&, const RegionFrames&) = default;
};

constexpr std::int64_t clampFrame(std::int64_t v, std::int64_t lo, std::int64_t hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Enforces PLY-001 against a sample of `frames` frames. frames <= 0 (no sample) gives the empty region {0, 0, 0}.
// Start wins over length, length over loop start: a start past the end keeps the last frame playable.
constexpr RegionFrames clampRegion(RegionFrames r, std::int64_t frames) noexcept
{
    if (frames <= 0)
        return {0, 0, 0};
    r.start = clampFrame(r.start, 0, frames - 1);
    r.length = clampFrame(r.length, 1, frames - r.start);
    r.loopStart = clampFrame(r.loopStart, r.start, r.start + r.length - 1);
    return r;
}

constexpr bool validRegion(const RegionFrames& r, std::int64_t frames) noexcept
{
    return frames > 0 && r.start >= 0 && r.length >= 1 && r.end() <= frames && r.loopStart >= r.start
        && r.loopStart < r.end();
}

// Frames per coarse encoder detent (PLY-002).
constexpr std::int64_t coarseStep(std::int64_t frames) noexcept
{
    return frames / kCoarseDetents > 1 ? frames / kCoarseDetents : 1;
}

// CC value 0–127 → frame position, linear over the sample (0 → 0, 127 → frames; clampRegion() keeps it in range).
constexpr std::int64_t ccToFrame(int cc, std::int64_t frames) noexcept
{
    const std::int64_t c = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    return frames <= 0 ? 0 : (c * frames + 63) / 127;
}

constexpr int clampGridCount(int count) noexcept
{
    return count < 1 ? 1 : (count > kMaxGridCount ? kMaxGridCount : count);
}

// Grid position n (1…count) → its frame: (n − 1)·frames / count, rounded down.
constexpr std::int64_t gridFrame(int position, int count, std::int64_t frames) noexcept
{
    count = clampGridCount(count);
    const std::int64_t n = position < 1 ? 1 : (position > count ? count : position);
    return frames <= 0 ? 0 : (n - 1) * frames / count;
}

// Frame → the nearest grid position (1…count); ties go to the earlier position.
constexpr int gridPosition(std::int64_t frame, int count, std::int64_t frames) noexcept
{
    count = clampGridCount(count);
    if (frames <= 0)
        return 1;
    frame = clampFrame(frame, 0, frames);
    // Nearest n − 1 = round(frame·count / frames), computed without overflow for any sample length below 2^56.
    const std::int64_t scaled = frame * count;
    std::int64_t n = scaled / frames;
    if ((scaled % frames) * 2 > frames)
        ++n;
    return static_cast<int>(n + 1 > count ? count : n + 1);
}

// The frame of the grid position nearest to `frame` (GRID SNAP on: markers snap to the grid).
constexpr std::int64_t snapToGrid(std::int64_t frame, int count, std::int64_t frames) noexcept
{
    return gridFrame(gridPosition(frame, count, frames), count, frames);
}
} // namespace core::playback
