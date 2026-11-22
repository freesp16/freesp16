// Slices between the document, the engine and the pads (SLC-001…005, SLC-010; threading.md "Object swap
// protocol"). Message thread only.
//
//   SliceSync     every edit of `scene[s].track[t].slices` (core/commands/slice), every newly installed track sample
//                 and every project open builds the track's slice table (≤ 65 offsets, < 10 µs) and posts it as
//                 ObjectSwap{SwapSlot::SliceMap, s, t}; the engine installs it at a block start and retires the old
//                 one. `frames` = the frame count of the buffer the track plays (0 = none: the slot is cleared).
//   SliceLive     what the pads did in SLICE pad mode, for the UI: the last-played slice per track (step entry writes
//                 it as the slice-number lock, SLC-005; SET SLICE's SLICE No. follows it), the pad hits waiting for the
//                 engine's RecordedHit (real-time recording writes their slice lock: the adapter on the UI side —
//                 the slice of a pad press is fixed by the model, so the message thread pairs it with the recorded
//                 hit of the same track, oldest first, within 1 s) and the hit times of the slice boxes.
#pragma once

#include "core/audio/SampleBuffer.h"
#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "engine/EngineApi.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace ui::slice
{
using FramesFn = std::function<std::int64_t(int scene, int track)>;

// Builds and posts the table of (scene, track); a refused post destroys it. False when the bridge refused.
bool postTable(engine::EngineBridge&, const core::model::Project&, int scene, int track, std::int64_t frames);
// Every scene × track (a project open, the offline render once its samples are loaded).
void postAll(engine::EngineBridge&, const core::model::Project&, const FramesFn& frames);

class SliceSync
{
public:
    SliceSync(engine::EngineBridge& bridge, FramesFn frames) : bridge_(bridge), frames_(std::move(frames)) {}
    void onChange(const core::ChangeSet&, const core::model::Project&); // the CommandBus bridge hook
    void onInstalled(int scene, int track, std::int64_t frames, const core::model::Project&); // a new sample
    void syncAll(const core::model::Project& p) { postAll(bridge_, p, frames_); }
    std::int64_t frames(int scene, int track) const { return frames_ ? frames_(scene, track) : 0; }

private:
    engine::EngineBridge& bridge_;
    FramesFn frames_;
};

struct SliceLive
{
    static constexpr int kPending = 16;
    static constexpr double kPairMs = 1000.0;

    std::array<int, 16> lastSlice {};        // per track, −1 = none yet
    std::array<double, 16> lastHitMs {};     // per track
    std::array<double, 64> sliceHitMs {};    // the selected track's slice boxes (SET SLICE flashes the hit one)
    int lastTrack = -1;
    struct Pending
    {
        std::int8_t track, slice;
        double ms;
    };
    std::array<Pending, kPending> pending {};
    int numPending = 0;

    // The buffer track (scene, track) plays (the app installs it: TrackSampleSync), for SET SLICE's zero-crossing
    // snap (SLC-006); unset or null = no snap.
    std::function<const core::audio::SampleBuffer*(int scene, int track)> buffer;

    SliceLive() { lastSlice.fill(-1); }
    // A SLICE pad mode press that played `slice` (≥ 0) on `track`; `recording` = the engine records it (REC armed and
    // the transport running): it then waits for its RecordedHit.
    void onHit(int track, int slice, double nowMs, bool recording = true) noexcept;
    // The slice of the oldest pad hit of `track` younger than kPairMs (consumed), for a RecordedHit.
    std::optional<int> takeRecorded(int track, double nowMs) noexcept;
};
SliceLive& sliceLive(); // the app's one instance (the screens read it, the document writes it)

// The slice pad `pad` plays on track (scene, track) of `p` over `frames` frames (−1 = none: silent). Without a sample
// length (frames ≤ 0) the stored grid count decides (the engine's default view of an unsliced sample is 16).
int padSlice(const core::model::Project& p, int scene, int track, int pad, std::int64_t frames) noexcept;
// Slices of the track (the point list's count; the grid count plus added points without a sample length).
int sliceCount(const core::model::Project& p, int scene, int track, std::int64_t frames) noexcept;
} // namespace ui::slice
