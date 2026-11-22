// The compiled-in controller profiles (Profile.h): byte-for-byte the files in resources/controllers/
// (tests/unit/controllers/test_controller_profiles.cpp keeps them equal; edit the JSON file, then paste it here).
#include "io/controllers/profiles/Profile.h"

namespace io::controllers
{
namespace
{
constexpr std::string_view kText0 = R"json({
  "$description": "Generic 16 + 8 (03-ui/control-surface.md UI-131): 16 pads and 8 endless encoders on one channel (10), the fallback for any port marked as a controller. Details the spec leaves open (our choice): relative CCs are two's complement; note 55 held = ext.steps (pad bank, UI-133); pad lamps = note-on on the pad's note with the velocity of the nearest colour of the rgb palette (0 off, 1-16 the track palette, then white, red, green, blue, amber); PLAY / STOP / REC lamps = CC 115 / 116 / 117 value 127 on, 0 off.",
  "schema": 1,
  "id": "generic-16-8",
  "name": "Generic 16 + 8",
  "ports": "*",
  "channel": 10,
  "padBank": true,
  "in": [
    {"note": 36, "count": 16, "control": "pad.1", "aftertouch": true},
    {"cc": 70, "count": 6, "enc": "twos", "control": "enc.1"},
    {"cc": 76, "enc": "twos", "control": "sel"},
    {"cc": 77, "enc": "twos", "control": "main.volume", "scale": 8},
    {"note": 52, "control": "sel.push"},
    {"note": 53, "control": "back"},
    {"note": 54, "control": "home"},
    {"note": 55, "control": "ext.steps"},
    {"mmc": "play", "control": "play"},
    {"mmc": "stop", "control": "stop"},
    {"mmc": "record", "control": "rec"},
    {"cc": 115, "control": "play"},
    {"cc": 116, "control": "stop"},
    {"cc": 117, "control": "rec"},
    {"cc": 118, "control": "shift"},
    {"pitchbend": true, "control": "strip"},
    {"cc": 119, "enc": "contact", "control": "strip"}
  ],
  "palettes": {
    "rgb": ["000000", "FF7E72", "FD8358", "F68C36", "EA9602", "D9A100", "C3AC00", "94BB3B", "51C672", "00CA9B",
            "00C7D2", "3FB3FF", "88A2FF", "B394FF", "D787E9", "F07EC2", "FE7B94", "FFFFFF", "FF1E28", "28F03C",
            "2050FF", "FFB020"],
    "mono": ["000000:0", "FFFFFF:127"]
  },
  "lamps": [
    {"control": "pad.1", "count": 16, "note": 36, "palette": "rgb"},
    {"control": "play", "cc": 115, "palette": "mono"},
    {"control": "stop", "cc": 116, "palette": "mono"},
    {"control": "rec", "cc": 117, "palette": "mono"}
  ]
}
)json";
constexpr std::string_view kText1 = R"json({
  "$description": "Reference sampler MIDI OUT (03-ui/control-surface.md UI-134): the reference hardware unit itself as a controller. Pads arrive as the track's note, base note C5 = 60, pad n = 60 + n - 1 (any channel: the unit sends each track on its own channel); the six knobs of the MIDI CC1 page arrive as pitch bend, channel aftertouch, CC 7, 1, 2, 10 and of the CC2 page as CC 71-76, absolute, turned into detents by difference (UI-132); Start / Stop / Continue = play / stop / play. Chosen by hand on the CONTROLLERS page (no port pattern). No lamps (the unit's MIDI IN does not light its pads).",
  "schema": 1,
  "id": "reference-sampler-midi-out",
  "name": "Reference sampler MIDI OUT",
  "channel": 0,
  "in": [
    {"note": 60, "count": 16, "control": "pad.1"},
    {"pitchbend": true, "control": "enc.1"},
    {"pressure": true, "control": "enc.2"},
    {"cc": 7, "control": "enc.3"},
    {"cc": 1, "control": "enc.4"},
    {"cc": 2, "control": "enc.5"},
    {"cc": 10, "control": "enc.6"},
    {"cc": 71, "count": 6, "control": "enc.1"},
    {"realtime": "start", "control": "play"},
    {"realtime": "stop", "control": "stop"},
    {"realtime": "continue", "control": "play"}
  ]
}
)json";

constexpr BuiltinProfileText kTexts[] = {{"generic-16-8.json", kText0}, {"reference-sampler-midi-out.json", kText1}};
} // namespace

std::span<const BuiltinProfileText> builtinProfileTexts() noexcept
{
    return kTexts;
}
} // namespace io::controllers
