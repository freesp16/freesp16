// What SAMPLING/EDIT and LIVE SAMPLING need of the sampler (services::sampling::SamplingSession; SMP-001…010,
// SMP-020…027): the capture hub (ring + status, read by the screens' waveforms and meters), REC / stop, live arm /
// disarm / nudge / save / unload, and the take's document side — the take on the track as `RECORDED TEMP.` (SMP-010),
// SAVE SELECTION / SAVE & REPLACE (SMP-009), discard, the EDIT list's processing (NORMALIZE REN-010, FADE IN / OUT; our
// choice) and BAR ⇄ BPM (REN-021). The composition root installs the product host (app/M5Hosts.cpp); tests install a
// fake. With none installed the screens show their empty state and REC does nothing. Message thread only; nothing here
// allocates per frame.
#pragma once

#include "core/util/sampling/CaptureHub.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ui::m5
{
enum class TakeProcess : std::uint8_t
{
    Normalize, // REN-010: the selection's peak to −1.0 dBFS (a new file, assigned with an undoable LOAD)
    FadeIn,    // a 10 ms fade at the selection's start
    FadeOut    // … at its end
};

using HostDone = std::function<void(bool ok, std::string message)>;

class SamplingHost
{
public:
    virtual ~SamplingHost() = default;
    // The capture hub (null until the first REC / arm allocated the ring).
    virtual const core::sampling::CaptureHub* hub() const = 0;

    // ---- SAMPLING/EDIT (scene / track 0-based) ----
    virtual bool record(int scene, int track) = 0; // REC: standby (the ring is allocated on first use); false = refused
    virtual void stop(int scene, int track) = 0;   // REC again: stop / leave standby
    virtual void drop(int scene, int track) = 0;   // the pad again: the take is thrown away (standby or recording)
    virtual bool unsaved(int scene, int track) const = 0; // the track plays an unsaved take (RECORDED TEMP.)
    virtual std::string proposedName(int scene, int track) const = 0; // "Beat_120bpm_2bar_01" (UID-780)
    // SAVE (keep the track's sample) / SAVE & REPLACE (the saved file goes on the track), frames [start, end).
    virtual void saveSelection(int scene, int track, std::int64_t start, std::int64_t end, const std::string& name,
                               bool replace, HostDone done) = 0;
    virtual void discard(int scene, int track) = 0; // the replaced sample comes back (SMP-010)
    virtual void process(int scene, int track, TakeProcess, std::int64_t start, std::int64_t end, HostDone done) = 0;
    // REN-021: the take's length in bars and its tempo (false = unknown); setTempo writes both (sidecar).
    virtual bool tempo(int scene, int track, double& bars, double& bpm) const = 0;
    virtual void setTempo(int scene, int track, double bars, double bpm) = 0;

    // ---- LIVE SAMPLING ----
    virtual bool liveArm(int scene, int track) = 0;
    virtual void liveDisarm(int scene, int track) = 0;
    virtual void setNudge(std::int32_t frames) = 0;
    virtual bool liveUnsaved() const = 0; // a live take on a track, not saved yet (UNSAVED TAKE badge, UID-789)
    virtual int liveScene() const = 0;    // scene owning the live take (0-based)
    virtual int liveTrack() const = 0;    // its track (0-based), −1 = none
    bool hasUnsavedTake(int scene, int track) const
    {
        return unsaved(scene, track) || (liveUnsaved() && liveScene() == scene && liveTrack() == track);
    }
    virtual void saveLive(int saveTo, HostDone done) = 0;
    virtual bool unloadLive() = 0; // SMP-025 UNLOAD: remove the take from its original slot

    virtual std::uint32_t revision() const = 0; // +1 whenever a take, a save or a discard changed something

    static SamplingHost* current() noexcept;
    static void install(SamplingHost*) noexcept;
};
} // namespace ui::m5
