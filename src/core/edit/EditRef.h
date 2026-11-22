// What an edit addresses (spec 02-features/editing-and-undo.md "Edit objects"). Indices are 0-based.
//
//   Track               scene, track: the track's sound settings (playback, envelope, LFO, insert FX, send, MIDI, …)
//   TrackWithSequences  scene, track: the same plus the track's column (steps, locks, pattern mute/solo) in all 16
//                       patterns of the scene
//   Sequence            scene, pattern, track: one track's 64 steps (triggers + locks) and its mute/solo in one pattern
//   Pattern             scene, pattern: all 16 tracks' steps, length, mutes and solos
//   Scene               scene: everything in the scene
//   Steps               scene, pattern, track (kAllTracks = all 16), stepMask: the steps of bit s (EDT-005)
//   Page                as Steps with one 16-step page (pageMask(n)): "copy whole bar" (EDT table, P1)
//
// As a paste destination, Steps / Page use the lowest set bit of stepMask as the target step (hold PASTE + target
// step, EDT-005; 0 when no bit is set). Families (EDT-011 "the kind must match"): Track and TrackWithSequences are one
// family (the clipboard's kind decides whether the sequences come along), Steps and Page are one family.
#pragma once

#include <cstdint>

namespace core::edit
{
enum class ObjectKind : std::uint8_t { Track, TrackWithSequences, Sequence, Pattern, Scene, Steps, Page };

inline constexpr std::uint8_t kAllTracks = 0xFF;

struct EditRef
{
    ObjectKind kind = ObjectKind::Scene;
    std::uint8_t scene = 0, pattern = 0, track = 0;
    std::uint64_t stepMask = 0;
    friend bool operator== (const EditRef&, const EditRef&) = default;
};

enum class Family : std::uint8_t { Track, Sequence, Pattern, Scene, Steps };
constexpr Family familyOf(ObjectKind k) noexcept
{
    switch (k)
    {
        case ObjectKind::Track:
        case ObjectKind::TrackWithSequences: return Family::Track;
        case ObjectKind::Sequence: return Family::Sequence;
        case ObjectKind::Pattern: return Family::Pattern;
        case ObjectKind::Scene: return Family::Scene;
        case ObjectKind::Steps:
        case ObjectKind::Page: break;
    }
    return Family::Steps;
}

// Bits of steps [16 n, 16 n + 16) (page n = 0…3).
constexpr std::uint64_t pageMask(int page) noexcept
{
    return page < 0 || page > 3 ? 0 : std::uint64_t {0xFFFF} << (16 * page);
}
// Bits of steps [first, first + count), clipped to 0…63.
constexpr std::uint64_t stepRange(int first, int count) noexcept
{
    std::uint64_t m = 0;
    for (int s = first < 0 ? 0 : first; s < first + count && s < 64; ++s)
        m |= std::uint64_t {1} << s;
    return m;
}

constexpr EditRef sceneRef(int scene) noexcept
{
    return {ObjectKind::Scene, static_cast<std::uint8_t>(scene & 15), 0, 0, 0};
}
constexpr EditRef patternRef(int scene, int pattern) noexcept
{
    return {ObjectKind::Pattern, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(pattern & 15), 0, 0};
}
constexpr EditRef trackRef(int scene, int track, bool withSequences) noexcept
{
    return {withSequences ? ObjectKind::TrackWithSequences : ObjectKind::Track, static_cast<std::uint8_t>(scene & 15), 0,
            static_cast<std::uint8_t>(track & 15), 0};
}
constexpr EditRef sequenceRef(int scene, int pattern, int track) noexcept
{
    return {ObjectKind::Sequence, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(pattern & 15),
            static_cast<std::uint8_t>(track & 15), 0};
}
// track = kAllTracks (or any value > 15) for all 16 tracks.
constexpr EditRef stepsRef(int scene, int pattern, int track, std::uint64_t stepMask) noexcept
{
    return {ObjectKind::Steps, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(pattern & 15),
            track < 0 || track > 15 ? kAllTracks : static_cast<std::uint8_t>(track), stepMask};
}
constexpr EditRef pageRef(int scene, int pattern, int track, int page) noexcept
{
    EditRef r = stepsRef(scene, pattern, track, pageMask(page));
    r.kind = ObjectKind::Page;
    return r;
}
} // namespace core::edit
