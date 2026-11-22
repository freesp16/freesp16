// E4 — MIDI learn (MID-034: any CC / note to any parameter path, stored per project, on top of the fixed map; FX-088:
// Send FX and Master FX parameters, which have no fixed CC, become reachable here). Behind the
// feature flag `ext.midi.learn` (off by default). Message thread; pure (no I/O, no threads).
//
//   bindings    LearnBinding {channel, status, number} → {ParamSpec, scope, lo, hi}: one per source message, ≤ 128,
//               stored per project in Project::extras "native.ext.midi.learn" (JSON array, ParamSpec ids by name so a
//               file survives catalogue growth; the native codec keeps the key, a file written with the flag on loads
//               with it off and keeps the bindings)
//   apply       a CC value 0–127 maps linearly onto [lo, hi] of the target's 0–1 range (integral types rounded); a note
//               on sets hi, a note off lo (momentary); after the fixed map: a learned source never shadows a
//               fixed CC (learning one is refused with the reason)
//   targets     every user-editable ParamSpec: not internal (storage "internal.*"), not a preference (Global: not per
//               project), not this extension's carrier; Send / Master FX parameters included (FX-088) and they stay
//               non-lockable (FX-094: a learned CC sets the stored value only, like an edit)
//   learn       LearnSession: arm a target (+ scope = the parameter path's scene / track), the next CC or note from a
//               port not marked `controller` (the controller profiles own those) becomes the binding
//
// The live MIDI path (no hook exists in the MIDI-in path): the fixed CC table (core/util/midi/CcTable.h,
// published through MidiHub::setCcTable) is replaced while the flag is on by an overlay that routes every CC the fixed
// map leaves free to the carrier ParamSpec `ext.midi.learn.cc` (internal); the engine applies a carrier CC like any
// other (a no-op there) and reports it to the UI as UiEvent::Param with value = cc × 128 + value, scope = the track
// whose receive channel it arrived on (decodeCarrier); the message thread applies the bindings through the same
// model path as the fixed CCs. Hence: CCs only (notes have no UI report), on channels a Sample / Thru track receives
// (MID-030's own rule), one UI frame of latency.
#pragma once

#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "core/util/Result.h"
#include "core/util/midi/CcTable.h"
#include "core/util/midi/MidiWire.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace services::midilearn
{
inline constexpr std::string_view kCarrierId = "ext.midi.learn.cc";   // internal ParamSpec (catalogue ext_midi_learn)
inline constexpr std::string_view kExtrasKey = "native.ext.midi.learn"; // Project::extras
inline constexpr std::uint8_t kStatusCc = 0xB0, kStatusNote = 0x90;

// A learned source and its target, plus the scope that makes a parameter id a parameter *path* (scene / track).
struct LearnBinding
{
    std::uint8_t channel = 1;          // 1–16
    std::uint8_t status = kStatusCc;   // 0xB0 CC, 0x90 note
    std::uint8_t number = 0;           // CC or note number 0–127
    core::ParamId target {};
    std::uint16_t scope = 0;           // the ParamChange scope of the path ((scene << 4) | track, scene, 0xFFFF …)
    float lo = 0, hi = 1;              // 0–1 of the target's range
    friend bool operator== (const LearnBinding&, const LearnBinding&) = default;
};

struct Message
{
    std::uint8_t channel = 1; // 1–16
    std::uint8_t status = kStatusCc; // 0xB0, 0x90 (velocity 0 = off), 0x80
    std::uint8_t number = 0, value = 0;
};

struct Applied
{
    const core::ParamSpec* spec = nullptr;
    std::uint16_t scope = 0;
    float value = 0; // the target's own units, clamped
};

bool learnable(const core::ParamSpec&) noexcept;
std::vector<core::ParamId> learnTargets(const core::ParamRegistry&);

class LearnMap
{
public:
    static constexpr std::size_t kMaxBindings = 128;

    // Adds or replaces the binding of the same source. Refused: an unknown / not learnable target, lo/hi outside 0–1,
    // a full map, or (fixed != null) a CC the fixed map owns.
    core::Result<void> add(const LearnBinding&, const core::ParamRegistry&, const core::midi::CcTable* fixed = nullptr);
    bool remove(std::uint8_t channel, std::uint8_t status, std::uint8_t number);
    void clear() { bindings_.clear(); }
    std::span<const LearnBinding> bindings() const noexcept { return bindings_; }
    bool empty() const noexcept { return bindings_.empty(); }

    // The bindings of `m` (appended to out); returns how many applied.
    int apply(const Message& m, const core::ParamRegistry&, std::vector<Applied>& out) const;

    std::string toJson(const core::ParamRegistry&) const; // [{"ch":1,"st":176,"no":20,"param":"…","scope":0,"lo":0,"hi":1}]
    static core::Result<LearnMap> fromJson(std::string_view, const core::ParamRegistry&, std::string* warnings = nullptr);
    static LearnMap load(const core::model::Project&, const core::ParamRegistry&, std::string* warnings = nullptr);

    friend bool operator== (const LearnMap&, const LearnMap&) = default;

private:
    std::vector<LearnBinding> bindings_;
};

class LearnSession
{
public:
    void arm(core::ParamId target, std::uint16_t scope, float lo = 0, float hi = 1) noexcept;
    void cancel() noexcept { armed_ = false; }
    bool armed() const noexcept { return armed_; }
    core::ParamId target() const noexcept { return target_; }
    // The binding `m` makes while armed (then disarmed); nullopt when not armed, from a controller port, or a note off.
    std::optional<LearnBinding> capture(const Message& m, bool fromControllerPort) noexcept;

private:
    bool armed_ = false;
    core::ParamId target_ {};
    std::uint16_t scope_ = 0;
    float lo_ = 0, hi_ = 1;
};

// The overlay: `fixed` with every CC it leaves free (kind None) routed to `carrier` (Linear, cc × 128 + value).
void buildOverlay(const core::midi::CcTable& fixed, core::ParamId carrier, core::midi::CcTable& out) noexcept;
bool fixedCc(const core::midi::CcTable& fixed, int cc) noexcept; // the fixed map owns this CC number
// A carrier UiEvent back to the message: the channel is the receive channel of the event's track.
std::optional<Message> decodeCarrier(const core::midi::UiEvent&, core::ParamId carrier, const core::model::MidiSettings&);
} // namespace services::midilearn
