// The sequencer's performance half in the engine block (spec 02-features/sequencer.md SEQ-030 / SEQ-035): ratchets and
// transpose, held by engine::Engine next to step 6.
//
//   parameters (ParamChange, scope = (scene << 4) | track; core/params/catalogue/sequencer.cpp, bound to
//              ParamTarget::Sequencer so EngineBridge::post accepts them)
//     seq.retrigCount   1–16          the track's RETRIGGER CNT (a step's slot-16 lock overrides it)
//     seq.retrigSpeed   enum 0–5      the track's RETRIGGER SPD (slot-17 lock overrides it)
//     seq.transpose     −24…+24 st    the track's TRANSPOSE
//   process()     after the sequencer pushed this block's steps into the router, before router.process(): walks the
//                 block's pending events in frame order — a sequencer FULL / HALF step of track t emits t's repeats
//                 due before it, then re-arms t (count 1 = cancel); a FULL hit (any source) on a track in a choke
//                 group cancels the other members' repeats from its frame on (TRK-020: the choke stops them); a Stop
//                 event cancels its track — then pushes the rest of the block's repeats. Transport stopped (STOP,
//                 pause) or a start edge: everything pending is dropped; a scene switch drops the old scene's
//                 ratchets at the switch frame (our choice; a pattern switch inside the scene keeps them).
//   afterRouter() a repeat is itself a FULL hit: one that chokes another track cancels that track's later repeats
//                 (from the next block on, our choice: repeats of two tracks of one choke group within one block are
//                 not cut inside that block).
//   publishMods() transpose as ModSource::Transpose, ModKind::Offset on playback.pitch (pitchOffset01(st · 100):
//                 ±24 st = ±0.5 of the normalised range), once per change and after a scene switch; 0 st clears it.
//                 Engine: the ModMatrix, after the LFO and before applyBlock — the matrix sums it with the
//                 LFO / ribbon / bend and LfoStage's sink clamps stored pitch + modulation to ±24 st in one place.
//                 MIDI tracks read transpose().
//
// Budget: ≤ 30 cycles per repeat, a block without ratchets ≈ one scan of the pending events (≤ 64).
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/modulation/ModMatrix.h"
#include "engine/sequencer/perf/Retrigger.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/trigger/TriggerRouter.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::sequencer::perf
{
// What publishMods() needs of the modulation matrix (engine::modulation::ModMatrix satisfies it).
template <class M>
concept TransposeMatrix = requires(M& m, int track, core::ParamId id, float v) {
    { m.set(track, id, modulation::ModSource::Transpose, modulation::ModKind::Offset, v) } -> std::convertible_to<bool>;
    m.clear(track, id, modulation::ModSource::Transpose);
};

class PerfStage
{
public:
    static constexpr int kScenes = 16;

    RetriggerScheduler retrig;

    PerfStage() noexcept { resetValues(); }

    // ---- message thread (engine set-up) ----
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                    bridge::ParamBindings& bindings) noexcept
    {
        if (! resolve)
            return;
        count_ = resolve("seq.retrigCount");
        speed_ = resolve("seq.retrigSpeed");
        transpose_ = resolve("seq.transpose");
        pitch_ = resolve("playback.pitch");
        for (const auto id : {count_, speed_, transpose_})
            bindings.bind(id, bridge::ParamTarget::Sequencer);
    }
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        retrig.reset();
        dirty_ = 0xFFFF; // the matrix / voices may have been reset: publish every track again
    }
    core::ParamId pitchParam() const noexcept { return pitch_; }

    // ---- audio thread ----
    // A ParamChange for this stage: applied, true; anything else: false.
    bool onParam(const ParamChange& c) noexcept
    {
        const auto id = c.param;
        if (! id.valid() || c.scope > 0xFF || ! (id == count_ || id == speed_ || id == transpose_))
            return false;
        const int sc = c.scope >> 4 & 15, t = c.scope & 15;
        const long v = c.value < 0.0f ? static_cast<long>(c.value - 0.5f) : static_cast<long>(c.value + 0.5f);
        auto& tr = tracks_[static_cast<std::size_t>(sc)][static_cast<std::size_t>(t)];
        if (id == count_)
            tr.count = static_cast<std::uint8_t>(v < 1 ? 1 : (v > kMaxRetrigCount ? kMaxRetrigCount : v));
        else if (id == speed_)
            tr.speed = static_cast<std::uint8_t>(v < 0 ? 0 : (v >= kRetrigSpeeds ? kRetrigSpeeds - 1 : v));
        else
        {
            const auto st = static_cast<std::int8_t>(v < -24 ? -24 : (v > 24 ? 24 : v));
            if (st != tr.transpose && sc == scene_)
                dirty_ = static_cast<std::uint16_t>(dirty_ | (1u << t));
            tr.transpose = st;
        }
        return true;
    }

    void process(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                 trigger::TriggerRouter& router) noexcept;
    void afterRouter(const trigger::TriggerRouter& router) noexcept;

    template <class Matrix>
        requires TransposeMatrix<Matrix>
    void publishMods(Matrix& m) noexcept
    {
        for (std::uint32_t d = dirty_; d != 0; d &= d - 1)
        {
            const int t = __builtin_ctz(d);
            const int st = tracks_[static_cast<std::size_t>(scene_)][static_cast<std::size_t>(t)].transpose;
            if (st == 0)
                m.clear(t, pitch_, modulation::ModSource::Transpose);
            else if (! m.set(t, pitch_, modulation::ModSource::Transpose, modulation::ModKind::Offset,
                             modulation::pitchOffset01(static_cast<float>(st) * 100.0f)))
                continue; // no free destination slot: try again next block
            dirty_ = static_cast<std::uint16_t>(dirty_ & ~(1u << t));
        }
    }

    // ---- state (audio thread; tests after the block) ----
    int transpose(int scene, int track) const noexcept { return tracks_[scene & 15][track & 15].transpose; }
    int transpose(int track) const noexcept { return transpose(scene_, track); } // current scene
    RetrigSpec trackRetrig(int scene, int track) const noexcept
    {
        const auto& tr = tracks_[scene & 15][track & 15];
        return {tr.count, tr.speed};
    }
    // The spec a sequencer step plays with: its slot-16 / 17 locks, else the track's values.
    RetrigSpec specOf(int scene, const trigger::TriggerEvent& step) const noexcept;
    int repeatsThisBlock() const noexcept { return emitted_; }
    std::uint16_t transposeDirty() const noexcept { return dirty_; }

private:
    struct TrackValues
    {
        std::uint8_t count = 1, speed = kDefaultRetrigSpeed;
        std::int8_t transpose = 0;
    };
    void resetValues() noexcept
    {
        for (auto& s : tracks_)
            s.fill(TrackValues {});
    }
    void cutGroup(const trigger::TriggerRouter& router, int scene, const trigger::TriggerEvent& e) noexcept;

    core::ParamId count_ {}, speed_ {}, transpose_ {}, pitch_ {};
    double sampleRate_ = 44100.0;
    std::array<std::array<TrackValues, kTracks>, kScenes> tracks_;
    int scene_ = 0;
    std::uint16_t dirty_ = 0xFFFF;
    std::array<std::int8_t, kTracks> armedAt_ {}; // this block's arm frame per track (−1 none)
    int emitted_ = 0;
};
} // namespace engine::sequencer::perf
