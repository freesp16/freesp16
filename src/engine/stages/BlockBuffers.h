// The per-block working set every stage reads and writes (audio-engine.md "Per-block processing order"), next to the
// frozen engine::BlockContext (EngineApi.h). All of it is a plain member of engine::Engine: fixed size, 16-byte
// aligned, allocated once with the engine, never resized (NFR-020/021). Sized for the full machine — 16 tracks, the
// send bus, aux, master and every logical output port — even while the stages are empty.
//
// "active" masks let empty stages cost ~nothing: a track whose source is silent and whose inserts are idle has its
// bit clear and is skipped by the strip/bus sums (its buffer content is then unspecified, never read).
#pragma once

#include "engine/EngineApi.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace engine::stages
{
struct alignas(16) StereoBlock
{
    alignas(16) float l[kBlockFrames];
    alignas(16) float r[kBlockFrames];
    void clear() noexcept
    {
        std::memset(l, 0, sizeof(l));
        std::memset(r, 0, sizeof(r));
    }
};

constexpr int kPorts = static_cast<int>(OutPort::Count);
constexpr int kInsertsPerTrack = 2; // InsertFx1 → InsertFx2 (audio-engine.md step 8)

struct BlockBuffers
{
    std::array<StereoBlock, kTracks> track {}; // step 8: one planar stereo block per track
    std::uint32_t activeTracks = 0;            // bit t = track t produced audio this block
    StereoBlock mainBus {}, sendBus {}, aux {}, master {};
    bool sendActive = false, auxActive = false;
    const float* inputL = nullptr; // audio input (Thru tracks, input monitor), one block late; null = no input
    const float* inputR = nullptr;
    // Step 12: the block each logical output port plays this block; null = silent (the adapter writes zeros).
    std::array<const float*, 2 * kPorts> port {};

    // The fields BlockContext (frozen) cannot carry. Filled by step 3 (transport: preCount),
    // step 5 (scene/pattern switcher: scene, pattern, patternStartPpq, boundary, sceneChanged); read by step 6
    // (sequencer), the mixer (pattern mute/solo) and step 14 (PlayheadFeed). One writer per field.
    struct Timing
    {
        std::uint8_t scene = 0, pattern = 0;   // current scene / pattern this block
        double patternStartPpq = 0.0;          // PPQ at which the current pattern's step 1 started
        struct Boundary
        {
            bool has = false;                  // a queued switch lands inside this block
            std::uint8_t frame = 0;            // 0–63
            double ppq = 0.0;
        } boundary;
        bool preCount = false;                 // recording pre-count running (transport plays no pattern)
        bool sceneChanged = false;             // the scene changed at the start or at the boundary of this block
    } timing;
};
} // namespace engine::stages
