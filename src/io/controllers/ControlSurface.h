// The logical control surface. Spec: 03-ui/control-surface.md — §2.1 the
// logical control set (UI-100/101, extension controls UI-102), §2.2 events (UI-110…118), lamps (UI-150…161),
// controller mappings (UI-130). Every input device — the hardware panel, the touch strip, the computer keyboard, MIDI
// controllers through profiles, the on-screen virtual panel — is a ControlSource producing ControlEvents in this one
// vocabulary; screens never know which device produced an event (UI-100). Devices that show lamps are LampSinks.
//
// Latency path (UI-118): a source that produces pad or MIDI-note hits pushes engine::TriggerNow into the
// engine::TriggerSink it was given *from its reader thread*, then queues the ControlEvent for the UI (selection, flash).
// The UI never re-triggers.
//
// Naming: the enum uses Pad1…Pad16, the spec and profile JSON use "pad.1"; controlFromName()/nameOf() map both.
#pragma once

#include "core/util/Registrar.h"
#include "engine/EngineApi.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace io
{
enum class Control : std::uint16_t { Pad1 = 0, Pad2, Pad3, Pad4, Pad5, Pad6, Pad7, Pad8, Pad9, Pad10, Pad11, Pad12,
                                     Pad13, Pad14, Pad15, Pad16,
                                     Step1 = 16, Step2, Step3, Step4, Step5, Step6, Step7, Step8, Step9, Step10,
                                     Step11, Step12, Step13, Step14, Step15, Step16,
                                     Bar1 = 32, Bar2, Bar3, Bar4,                                  // bar.1…bar.4
                                     PadModeTrack, PadModeMute, PadModeSlice, PadModeScale,        // padmode.*
                                     Scene, Pattern,
                                     Knob1 = 48, Knob2, Knob3, Knob4, Knob5, Knob6,               // enc.1…enc.6
                                     Selector, SelectorPush, Play, Stop, Rec, Shift, Back, Home,
                                     StripMode, StripHold,                                          // strip.mode/.hold
                                     AfsActive, AfsDrive, AfsCutoff, AfsResonance, AfsHpCutoff, MainVolume,
                                     // lamps only (UI-150…161)
                                     StripLed1, StripLed2, StripLed3, StripLed4, StripLed5, StripLed6, StripLed7,
                                     StripLed8, StripModePitch, StripModeRepeat, StripModeUser1, StripModeUser2, AfsOver,
                                     // extension controls (UI-102)
                                     ExtCopy, ExtPaste, ExtClear, ExtUndo, ExtRedo, ExtCue, ExtErase, ExtTap,
                                     ExtNudgeDown, ExtNudgeUp, ExtAccent, ExtMixer, ExtBrowse, ExtUtility, ExtProject,
                                     Strip,                                                         // absolute 0–127 + contact
                                     Screen,                                                        // LCD touch (Touch/Drag/Release)
                                     // DJS-1000 panel controls (not on the SP-16 panel; its NUDGE keys are ExtNudgeUp
                                     // and ExtNudgeDown, its PROJECT key ExtProject)
                                     PadModeHotSlice, TempoFader, TempoReset, TempoRange, Sync, Master, FxOn, FxLevel,
                                     PhonesLevel, LiveSampling, FxSelect,
                                     Count };

constexpr Control pad(int n1Based) noexcept { return static_cast<Control>(static_cast<int>(Control::Pad1) + n1Based - 1); }
constexpr Control step(int n1Based) noexcept { return static_cast<Control>(static_cast<int>(Control::Step1) + n1Based - 1); }
constexpr Control knob(int n1Based) noexcept { return static_cast<Control>(static_cast<int>(Control::Knob1) + n1Based - 1); }
constexpr bool isPad(Control c) noexcept { return c >= Control::Pad1 && c <= Control::Pad16; }
constexpr bool isStep(Control c) noexcept { return c >= Control::Step1 && c <= Control::Step16; }
constexpr bool isKnob(Control c) noexcept { return c >= Control::Knob1 && c <= Control::Knob6; }
constexpr int indexOf(Control c) noexcept // 1-based index within its group (pad.5 → 5), 0 for single controls
{
    return isPad(c) ? static_cast<int>(c) - static_cast<int>(Control::Pad1) + 1
         : isStep(c) ? static_cast<int>(c) - static_cast<int>(Control::Step1) + 1
         : isKnob(c) ? static_cast<int>(c) - static_cast<int>(Control::Knob1) + 1
                     : 0;
}

// Spec names (control-surface.md §2.1, "pad.1", "enc.3", "sel.push", "ext.undo") and enum names ("Pad1", "Knob3")
// both parse; nameOf() returns the spec name.
std::optional<Control> controlFromName(std::string_view name) noexcept;
std::string_view nameOf(Control) noexcept;

// ControlEvent::source values.
enum SourceId : std::uint8_t { kSourcePanel = 0, kSourceKeyboard = 1, kSourceVirtual = 2, kSourceStrip = 3, kSourceMidi = 16 /* + port */ };

struct ControlEvent {                 // UI-110…118
    enum class Kind : std::uint8_t { Press, Release, Turn, Value, Touch, Pressure } kind;   // Pressure: pad aftertouch
    Control control; std::int16_t value;   // velocity, detents, absolute value, strip position
    std::uint8_t source;                   // panel / keyboard / midi:<n> / virtual
    double timeMs;
    std::int16_t x = 0, y = 0;             // Touch on Control::Screen: logical 800 × 480 point (UI-110)
};

class ControlSource {                       // a device that produces events
public:
    virtual ~ControlSource() = default;
    virtual std::string_view id() const = 0;
    virtual void poll(std::vector<ControlEvent>& out) = 0;   // UI thread
    // Opens the device and starts its reader thread (message thread). False + reason when the device is absent;
    // the app logs "<ID> ok|FAILED <info>" and keeps running without it.
    virtual bool start(std::string& info) { (void) info; return true; }
    virtual void stop() {}
};
class LampSink {                            // lamp/LED feedback (UI-150…161)
public:
    virtual ~LampSink() = default;
    virtual void setLamp(Control, std::uint32_t rgb, std::uint8_t level) = 0;
    virtual void flush() = 0;
    // The unit's analog filter in use (ANALOG FILTER ACTIVE with PROCESSOR EXTERNAL) and afs.hpCutoff (0–1023): a sink
    // with the filter board drives its select line and its high-pass DAC from these (LampFrame::analogFilter, before
    // flush()).
    virtual void setAnalogFilter(bool /*inUse*/, int /*hpCutoff*/) {}
};

// One MIDI message → logical control mapping of a controller profile (UI-130, resources/controllers/*.json).
struct MidiBinding
{
    enum class Message : std::uint8_t { Note, Cc, CcRelative, PitchBend, Aftertouch } message;
    std::uint8_t channel;  // 1–16, 0 = any
    std::uint8_t number;   // note or CC number
    Control control;
};
struct ControllerProfile {                  // MIDI note/CC → Control mapping, JSON in resources/controllers/
    std::string_view id, name; std::span<const MidiBinding> bindings; };

// Device-independent control sources (MIDI with profiles, the virtual panel) register with
// APP_REGISTER_CONTROL_SOURCE(id, factory); platform-specific ones come from platform::Platform::controlSources().
using ControlSourceFactory = std::unique_ptr<ControlSource> (*)(engine::TriggerSink* triggers);
class ControlSourceRegistry
{
public:
    struct Entry
    {
        std::string_view id;
        ControlSourceFactory create;
    };
    static ControlSourceRegistry& builtin() noexcept
    {
        static ControlSourceRegistry registry;
        return registry;
    }
    bool add(std::string_view id, ControlSourceFactory create)
    {
        for (const auto& e : entries_)
            if (e.id == id)
                return false;
        entries_.push_back({id, create});
        return true;
    }
    const std::vector<Entry>& all() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};
} // namespace io

#define APP_REGISTER_CONTROL_SOURCE(id, factory)                                                                       \
    APP_STATIC_REGISTRAR(::io::ControlSourceRegistry::builtin().add(id, factory))
