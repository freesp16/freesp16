// Controller profiles (spec 03-ui/control-surface.md §2.4 UI-130…136): a JSON file
// that maps the MIDI messages of a pad controller to logical controls and logical lamps back to MIDI messages. Adding a
// controller is a JSON file, no code (05-architecture §5.7). Built-ins are compiled in (builtinProfiles(), the same
// text as resources/controllers/*.json — tests keep them equal); user profiles (MIDI learn) live in the data
// folder's Controllers/ (ControllerHub), never in resources.
//
// Schema v1 (our format; every member but "in" optional):
//   {"schema": 1, "id": "generic-16-8", "name": "Generic 16 + 8",
//    "ports": "*",                 port-name pattern: case-insensitive glob (* ?), alternatives separated by |;
//                                  "*" = the fallback for any controller port (UI-131); absent = chosen by hand only
//    "channel": 10,                default channel of the bindings, 1–16, 0 = any
//    "padBank": true,              UI-133: while the profile's `ext.steps` button is held, pads act as step.1…16
//    "in": [ binding … ],
//    "palettes": {"rgb16": ["000000", "FF7E72", …]},  index = the value sent (note-on velocity / CC value);
//                                  "RRGGBB:v" sends v instead of the index (a mono lamp lit by 127: "FFFFFF:127")
//    "lamps": [ lamp … ]}
//   binding: one message key — "note": n | "cc": n | "pitchbend": true | "pressure": true (channel aftertouch) |
//            "mmc": "play"|"stop"|"record"|"pause"|<command> | "realtime": "start"|"stop"|"continue" —
//            plus "control": <logical id> (03-ui/control-surface.md §2.1, or "ext.steps" = the pad-bank button),
//            optional "count": k (k consecutive numbers → k consecutive controls of a group: pad / step / bar / enc),
//            "channel", "enc" (encoding, below), "scale" (detents per step; ±n per detent for relative → absolute),
//            "aftertouch": true (poly aftertouch on these notes = PadPressure, UI-115)
//   encodings (UI-132): "note" (on/off; the default for notes), "absolute" (the default for cc / pressure: a button
//            presses at ≥ 64, an absolute control takes the value, an encoder turns by the difference, 127 → 0 wrap
//            ignored), "twos" (1–63 up, 65–127 down), "offset" (64 ± n), "signmag" (1–63 up, 65–127 = −(v − 64)),
//            "pitchbend" (14 bit; the default for pitchbend: strip 0–127 with contact while off centre, absolute
//            controls 0–1023, encoders by difference), "mmc", "realtime", "contact" (a CC ≥ 64 = finger on the strip)
//   lamp: "control" (a lamp of Part 3), optional "count", "note": n | "cc": n, "channel", "palette": <name>
#pragma once

#include "core/util/Result.h"
#include "io/controllers/ControlSurface.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace io::controllers
{
using core::Result;

enum class Encoding : std::uint8_t { Note, Absolute, TwosComplement, BinaryOffset, SignMagnitude, PitchBend14, Mmc,
                                     Realtime, Contact };
// A binding's job besides producing its control's events (`ext.steps` is not an io::Control).
enum class Function : std::uint8_t { Control, PadBank };

// MIDI status classes used by bindings and lamps (the high nibble of a channel message, or a system byte).
namespace status
{
inline constexpr std::uint8_t note = 0x90, cc = 0xB0, polyPressure = 0xA0, pressure = 0xD0, pitchBend = 0xE0,
                              mmc = 0xF0, realtime = 0xF8;
}

struct MidiBinding
{
    std::uint8_t status = status::note; // status class (see `status`)
    std::uint8_t channel = 0;           // 1–16, 0 = any
    std::uint8_t number = 0;            // note / CC number, MMC command, realtime byte (FA/FB/FC)
    Encoding enc = Encoding::Note;
    Control control = Control::Count;
    std::int16_t scale = 1;
    bool aftertouch = false;
    Function function = Function::Control;
};

struct LampMap
{
    Control control = Control::Count;
    std::uint8_t status = status::note, channel = 0, number = 0; // channel 0 = the profile channel (1 when that is any)
    std::uint8_t palette = 0;                                     // index into LoadedProfile::palettes
};

struct LoadedProfile
{
    std::string id, name, portPattern; // empty pattern = never matched automatically
    std::uint8_t channel = 0;
    std::vector<MidiBinding> in;
    std::vector<LampMap> lamps;
    std::vector<std::vector<std::uint32_t>> palettes; // value sent << 24 | 0xRRGGBB
    std::vector<std::string> paletteNames;
    bool padBank = false;
    bool builtin = false;                             // compiled in (never written)
    bool fallback() const noexcept { return portPattern == "*"; }
};

inline constexpr int kMaxBindings = 512;

// Parses and validates a schema-v1 profile. Errors name the member ("in[3]: unknown control 'pad.17'").
Result<LoadedProfile> parseProfile(std::string_view json);
// The profile as schema-v1 JSON (bindings one per message, sorted members; parseProfile(toJson(p)) == p).
std::string toJson(const LoadedProfile&);

// The compiled-in profiles: Generic 16 + 8 (UI-131) and Reference sampler MIDI OUT (UI-134).
struct BuiltinProfileText
{
    std::string_view file; // resources/controllers/<file>
    std::string_view json;
};
std::span<const BuiltinProfileText> builtinProfileTexts() noexcept;
const std::vector<LoadedProfile>& builtinProfiles(); // parsed once (message thread)
inline constexpr std::string_view kGenericProfileId = "generic-16-8";
inline constexpr std::string_view kReferenceProfileId = "reference-sampler-midi-out";

// Port-name pattern match (case-insensitive glob with * and ?, alternatives separated by |).
bool matchesPort(std::string_view pattern, std::string_view portName) noexcept;
// The profile for a port: the first non-fallback profile whose pattern matches, else the first fallback, else null.
const LoadedProfile* profileForPort(std::span<const LoadedProfile> profiles, std::string_view portName) noexcept;

// Names used by the JSON (exposed for the MIDI learn page and the tests).
std::string_view encodingName(Encoding) noexcept;
} // namespace io::controllers
