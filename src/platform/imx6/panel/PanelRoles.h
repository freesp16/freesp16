// The imx6 panel's wire map as compiled tables: button roles, counters, analog words, pads, LCD touch, lamp channels.
// The same board is the SP-16 and the DJS-1000. They share the frame (128 bytes, header 0x01 in and 0x02 out,
// CRC-16/X-25 at 124), the button bits of most keys, the touch strip and the 32 RGB lamps (pads, then steps). The rest
// differs, so each unit has a PanelMap (panelMap()).
//
//   SP-16      the tables at namespace level. The same data is resources/controllers/panel.json (the documented
//              button-role table); tests/unit/panel/test_panel_roles.cpp checks that both agree. Source: the
//              sp16-emulator's documentation (emulator/panel_map.py E1-E11 and its 1.60 profile), never firmware code.
//   DJS-1000   namespace djs1000, worked out by observing its panel link. The touch calibration is the SP-16's until it
//              is checked on a unit.
//
// Device-independent (no POSIX): compiled into the device library and, by tests/unit/panel, into the host tests.
#pragma once

#include "io/controllers/ControlNames.h"
#include "io/controllers/ControlSurface.h"

#include <array>
#include <cstdint>
#include <span>

namespace platform::imx6::panel
{
// Which unit's panel: the board is the same, the panel and the codec differ (Imx6Platform finds it at start).
enum class Unit : std::uint8_t { Sp16, Djs1000 };
constexpr const char* unitModel(Unit u) noexcept { return u == Unit::Djs1000 ? "DJS-1000" : "TORAIZ SP-16"; }

constexpr int kFrameBytes = 128;
constexpr std::uint8_t kRxHeader = 0x01, kTxHeader = 0x02;
constexpr int kCrcOffset = 124;                         // CRC-16/X-25 over bytes 0…123, little-endian
constexpr int kPadPressureOffset = 0x06, kPadContactOffset = 0x3f;
constexpr int kTouchXOffset = 0x2c, kTouchYOffset = 0x2e;
constexpr int kTxMarkerOffset = 0x77;
constexpr std::uint8_t kTxMarker = 0xaa;
constexpr int kTxBrightnessOffset = 0x6e;                // PAD/BUTTON BRIGHTNESS 1…4 (NORMAL = 4)
// The standby flag: TX byte 0x67 bit 0 (the stock app's "transition Standby"). Once it is set the unit's power MCU (M1)
// cuts the power, so only LampBus::enterStandby() sets it (emulator panel_map.py TX_STANDBY_OFFSET, profile 1.60).
constexpr int kTxStandbyOffset = 0x67;
// HardMute: TX byte 0x66 bit 0 mutes the analog outputs at once (the Sub-MCU echoes it as RX byte 62 bit 0, which is
// therefore no key). Set from the power key on, before the audio stops (hardware notes: POWER-BUTTON-FIX).
constexpr int kTxHardMuteOffset = 0x66;
// The POWER/STANDBY key on the back: M1 reads it, M2 relays it as RX byte 5 bit 4 (panel_map.py, the service manual's
// key matrix). Pressing it (down, held 50 ms) asks the app to go to standby — on the press, not the release, as asked on
// the unit (the stock app's PowerManager waits for the release, button object 14). The bit is
// active-low on the hardware (1 = idle, 0 = pressed: hardware notes, confirmed on a unit) but active-high in the
// emulator (DecoderConfig::powerActiveLow, setting power_key=high). A level counts once it held kPowerDebounceMs.
// (Learning the idle level from the first frames does not work: on a unit those read the pressed level.)
constexpr int kPowerKeyByte = 5, kPowerKeyBit = 4;
constexpr double kPowerDebounceMs = 50.0;
constexpr int kRgbFullScale = 74;                       // E8: lamp bytes reach 0x4a at full

struct ButtonRole
{
    std::uint8_t byte, bit;
    io::Control control; // Control::Count = a system key with no logical control (USB STOP, power)
};
inline constexpr ButtonRole kButtons[] = {
    {1, 1, io::Control::SelectorPush}, {1, 2, io::Control::Back},        {1, 3, io::Control::Home},
    {1, 4, io::Control::PadModeTrack}, {1, 5, io::Control::PadModeMute}, {1, 6, io::Control::PadModeSlice},
    {1, 7, io::Control::PadModeScale}, {2, 0, io::Control::Shift},       {2, 1, io::Control::Rec},
    {2, 2, io::Control::Stop},         {2, 3, io::Control::Play},        {2, 4, io::Control::StripHold},
    {2, 5, io::Control::StripMode},    {2, 6, io::Control::Scene},       {2, 7, io::Control::Pattern},
    {3, 0, io::Control::Step1},        {3, 1, io::Control::Step2},       {3, 2, io::Control::Step3},
    {3, 3, io::Control::Step4},        {3, 4, io::Control::Step5},       {3, 5, io::Control::Step6},
    {3, 6, io::Control::Step7},        {3, 7, io::Control::Step8},       {4, 0, io::Control::Step9},
    {4, 1, io::Control::Step10},       {4, 2, io::Control::Step11},      {4, 3, io::Control::Step12},
    {4, 4, io::Control::Step13},       {4, 5, io::Control::Step14},      {4, 6, io::Control::Step15},
    {4, 7, io::Control::Step16},       {5, 0, io::Control::Bar1},        {5, 1, io::Control::Bar2},
    {5, 2, io::Control::Bar3},         {5, 3, io::Control::Bar4},        {5, 5, io::Control::AfsActive},
};
// System keys (control-surface.md 1.5, not in the logical set of UI-101): reported nowhere yet.
inline constexpr ButtonRole kSystemButtons[] = {{5, 6, io::Control::Count} /* usb_stop */,
                                                {kPowerKeyByte, kPowerKeyBit, io::Control::Count} /* power */};

// Role lookup by (byte 1…6, bit): Control::Count = no role.
struct ButtonGrid
{
    std::array<std::array<io::Control, 8>, 7> at {};
};
template <std::size_t N>
constexpr ButtonGrid makeGrid(const ButtonRole (&roles)[N]) noexcept
{
    ButtonGrid g {};
    for (auto& row : g.at)
        row.fill(io::Control::Count);
    for (const auto& b : roles)
        g.at[b.byte][b.bit] = b.control;
    return g;
}
inline constexpr ButtonGrid kButtonGrid = makeGrid(kButtons);

struct Counter
{
    std::uint8_t offset;
    std::int8_t sign; // wire sign of a clockwise detent
    io::Control control;
};
inline constexpr Counter kCounters[] = {
    {48, 1, io::Control::Knob1}, {50, 1, io::Control::Knob2}, {52, 1, io::Control::Knob3},
    {54, 1, io::Control::Knob4}, {56, 1, io::Control::Knob5}, {58, 1, io::Control::Knob6},
    {60, 1, io::Control::Selector},
};

struct AnalogWord
{
    std::uint8_t offset;
    bool inverted; // 1023 − word = clockwise more
    io::Control control;
};
inline constexpr AnalogWord kAnalog[] = {
    {38, true, io::Control::AfsDrive}, {40, false, io::Control::MainVolume}, {42, true, io::Control::AfsHpCutoff}};
constexpr int kAnalogFullScale = 1023;

// LCD touch calibration (panel_map.py LCD_TOUCH_CALIBRATION, the firmware's default as the emulator documents it):
// screenX = round((rawLeft − rawX) · spanX / (rawLeft − rawRight)) + offsetX, Y likewise; contact = raw point inside
// the inclusive rectangle (idle reads (0, 0), always outside).
struct TouchCalibration
{
    int rawLeft = 940, rawRight = 72, rawTop = 87, rawBottom = 866;
    int spanX = 720, spanY = 405, offsetX = 40, offsetY = 29;
    int minX = 45, maxX = 967, minY = 45, maxY = 908;
};

// TX lamp channels (E7): RGB channel 0x10 + k at bytes 1 + 3k, channel k at 0x31 + 3k; mono channels bit-packed.
constexpr int rgbOffset(int channel) noexcept
{
    return channel >= 0x10 ? 1 + 3 * (channel - 0x10) : 0x31 + 3 * channel;
}
struct MonoChannel
{
    std::uint8_t byte, bit, flagByte; // flagByte 0 = no dim flag; flag set = dim
};
inline constexpr MonoChannel kMono[35] = {
    {99, 4, 0x6a},  {99, 1, 0x6a},  {99, 2, 0x6a},  {99, 3, 0x6a},  {99, 7, 0},     {99, 6, 0x6a},  {99, 5, 0x6a},
    {100, 4, 0x6b}, {100, 6, 0x6b}, {100, 5, 0x6b}, {99, 0, 0x6a},  {100, 1, 0},    {100, 0, 0},    {100, 2, 0},
    {100, 3, 0},    {0x62, 0, 0},   {0x62, 1, 0},   {0x62, 2, 0},   {0x62, 3, 0},   {0x62, 4, 0},   {0x62, 5, 0},
    {0x62, 6, 0},   {0x62, 7, 0},   {0x65, 3, 0},   {0x65, 2, 0},   {0x65, 0, 0},   {0x61, 4, 0x69}, {0x61, 5, 0x69},
    {0x61, 6, 0x69}, {0x61, 7, 0x69}, {0x61, 0, 0x69}, {0x61, 1, 0x69}, {0x61, 2, 0x69}, {0x61, 3, 0x69}, {0x65, 1, 0},
};

struct LampRole
{
    io::Control control;
    bool rgb;
    std::uint8_t channel;
};
// pads, steps, mono keys and indicators, strip LEDs, BAR keys, bar position lamps
constexpr std::size_t kLampCount = 16 + 16 + 16 + 8 + 4 + 4;
inline constexpr auto kLamps = [] {
    std::array<LampRole, kLampCount> t {};
    std::size_t k = 0;
    for (int i = 1; i <= 16; ++i)
        t[k++] = {io::pad(i), true, static_cast<std::uint8_t>(16 + i - 1)};
    for (int i = 1; i <= 16; ++i)
        t[k++] = {io::step(i), true, static_cast<std::uint8_t>(i - 1)};
    const LampRole mono[] = {
        {io::Control::PadModeTrack, false, 0}, {io::Control::PadModeMute, false, 1},
        {io::Control::PadModeSlice, false, 2}, {io::Control::PadModeScale, false, 3},
        {io::Control::Rec, false, 5},          {io::Control::Stop, false, 6},
        {io::Control::Play, false, 7},         {io::Control::Scene, false, 8},
        {io::Control::Pattern, false, 9},      {io::Control::StripHold, false, 10},
        {io::Control::StripModeRepeat, false, 11}, {io::Control::StripModePitch, false, 12},
        {io::Control::StripModeUser1, false, 13},  {io::Control::StripModeUser2, false, 14},
        {io::Control::AfsActive, false, 23},   {io::Control::AfsOver, false, 34},
    };
    for (const auto& m : mono)
        t[k++] = m;
    for (int i = 0; i < 8; ++i)
        t[k++] = {static_cast<io::Control>(static_cast<int>(io::Control::StripLed1) + i), false,
                  static_cast<std::uint8_t>(15 + i)};
    const io::Control bars[] = {io::Control::Bar1, io::Control::Bar2, io::Control::Bar3, io::Control::Bar4};
    for (int i = 0; i < 4; ++i)
        t[k++] = {bars[i], false, static_cast<std::uint8_t>(26 + i)};
    for (int i = 1; i <= 4; ++i)
        t[k++] = {io::barPositionLamp(i), false, static_cast<std::uint8_t>(30 + i - 1)};
    return t;
}();

// ---- DJS-1000 ----

namespace djs1000
{
// Same bits as the SP-16 for SELECT, BACK, HOME, SHIFT, transport, HOLD, MODE, SCENE, PATTERN, the steps and the BAR
// keys. The four pad-mode keys are MUTE, HOT SLICE, SLICE and SCALE (no TRACK key). Byte 6 is the DJ section.
inline constexpr ButtonRole kButtons[] = {
    {1, 1, io::Control::SelectorPush}, {1, 2, io::Control::Back},            {1, 3, io::Control::Home},
    {1, 4, io::Control::PadModeMute},  {1, 5, io::Control::PadModeHotSlice}, {1, 6, io::Control::PadModeSlice},
    {1, 7, io::Control::PadModeScale}, {2, 0, io::Control::Shift},           {2, 1, io::Control::Rec},
    {2, 2, io::Control::Stop},         {2, 3, io::Control::Play},            {2, 4, io::Control::StripHold},
    {2, 5, io::Control::StripMode},    {2, 6, io::Control::Scene},           {2, 7, io::Control::Pattern},
    {3, 0, io::Control::Step1},        {3, 1, io::Control::Step2},           {3, 2, io::Control::Step3},
    {3, 3, io::Control::Step4},        {3, 4, io::Control::Step5},           {3, 5, io::Control::Step6},
    {3, 6, io::Control::Step7},        {3, 7, io::Control::Step8},           {4, 0, io::Control::Step9},
    {4, 1, io::Control::Step10},       {4, 2, io::Control::Step11},          {4, 3, io::Control::Step12},
    {4, 4, io::Control::Step13},       {4, 5, io::Control::Step14},          {4, 6, io::Control::Step15},
    {4, 7, io::Control::Step16},       {5, 0, io::Control::Bar1},            {5, 1, io::Control::Bar2},
    {5, 2, io::Control::Bar3},         {5, 3, io::Control::Bar4},            {5, 5, io::Control::ExtProject},
    {5, 6, io::Control::LiveSampling}, {5, 7, io::Control::FxSelect},        {6, 1, io::Control::ExtNudgeUp},
    {6, 2, io::Control::ExtNudgeDown}, {6, 3, io::Control::TempoReset},      {6, 4, io::Control::TempoRange},
    {6, 5, io::Control::Sync},         {6, 6, io::Control::Master},          {6, 7, io::Control::FxOn},
};
// System keys: USB STOP, power, and a switch of unknown purpose (byte 6 bit 0, 1 at rest).
inline constexpr ButtonRole kSystemButtons[] = {{1, 0, io::Control::Count} /* usb_stop */,
                                                {kPowerKeyByte, kPowerKeyBit, io::Control::Count} /* power */,
                                                {6, 0, io::Control::Count}};
inline constexpr ButtonGrid kButtonGrid = makeGrid(kButtons);

// Little-endian cumulative counters: the six knobs under the screen, then the rotary selector.
inline constexpr Counter kCounters[] = {
    {0x08, 1, io::Control::Knob1}, {0x0a, 1, io::Control::Knob2}, {0x0c, 1, io::Control::Knob3},
    {0x0e, 1, io::Control::Knob4}, {0x10, 1, io::Control::Knob5}, {0x12, 1, io::Control::Knob6},
    {0x14, 1, io::Control::Selector},
};

// The tempo fader's position (0 at its minus end. The word before it, 0x56, is its centre reference, about 512), FX
// LEVEL/DEPTH, and the headphones' level knob on the front. The DJS-1000 has no analog headphone volume: the platform
// applies that knob to PHONES (Imx6Platform.cpp).
inline constexpr AnalogWord kAnalog[] = {{0x58, false, io::Control::TempoFader},
                                          {0x5a, false, io::Control::FxLevel},
                                          {0x5c, false, io::Control::PhonesLevel}};

constexpr int kPadPressureOffset = 0x16, kPadContactOffset = 0x36; // contact above 59 while a pad is held
constexpr int kTouchXOffset = 0x5e, kTouchYOffset = 0x60;
// TX flags at 0x6c: bit 0 keeps the outputs muted, bit 1 asks for standby, bit 2 until the panel has answered.
constexpr int kTxFlagsOffset = 0x6c, kTxBrightnessOffset = 0x6d;
constexpr std::uint8_t kFlagMute = 0x01, kFlagStandby = 0x02, kFlagStartup = 0x04;

// Two-level lamps by lamp id: (byte, shift), two bits each, 0 off, 1 bright, 3 dim.
struct TwoBitLamp
{
    std::uint8_t byte, shift; // byte 0 = no such lamp
};
inline constexpr auto kTwoBit = [] {
    std::array<TwoBitLamp, 0x49> t {};
    const std::uint8_t ids[][3] = {
        {0x00, 0x69, 0}, {0x01, 0x6a, 6}, {0x02, 0x6b, 0}, {0x03, 0x6b, 2}, {0x04, 0x69, 2}, {0x05, 0x69, 4},
        {0x06, 0x69, 6}, {0x07, 0x67, 0}, {0x08, 0x67, 2}, {0x09, 0x67, 4}, {0x0a, 0x67, 6}, {0x0b, 0x65, 0},
        {0x0c, 0x63, 0}, {0x0d, 0x63, 2}, {0x0e, 0x63, 4}, {0x0f, 0x63, 6}, {0x10, 0x64, 0}, {0x11, 0x64, 2},
        {0x12, 0x64, 4}, {0x13, 0x64, 6}, {0x14, 0x66, 4}, {0x15, 0x66, 2}, {0x16, 0x68, 0}, {0x17, 0x66, 0},
        {0x18, 0x65, 2}, {0x19, 0x65, 4}, {0x1a, 0x65, 6}, {0x1b, 0x6a, 0}, {0x1c, 0x6a, 2}, {0x1d, 0x68, 6},
        {0x1e, 0x6a, 4}, {0x3f, 0x61, 0}, {0x40, 0x61, 2}, {0x41, 0x61, 4}, {0x42, 0x61, 6}, {0x43, 0x62, 0},
        {0x44, 0x62, 2}, {0x45, 0x62, 4}, {0x46, 0x62, 6}, {0x47, 0x68, 4}, {0x48, 0x68, 2},
    };
    for (const auto& e : ids)
        t[e[0]] = {e[1], e[2]};
    return t;
}();

// pads, steps, keys and indicators, MODE's four lamps, strip LEDs, BAR keys, bar position lamps
constexpr std::size_t kLampCount = 16 + 16 + 17 + 4 + 8 + 4 + 4;
inline constexpr auto kLamps = [] {
    std::array<LampRole, kLampCount> t {};
    std::size_t k = 0;
    for (int i = 1; i <= 16; ++i)
        t[k++] = {io::pad(i), true, static_cast<std::uint8_t>(16 + i - 1)};
    for (int i = 1; i <= 16; ++i)
        t[k++] = {io::step(i), true, static_cast<std::uint8_t>(i - 1)};
    const LampRole keys[] = {
        {io::Control::PadModeMute, false, 0x17},  {io::Control::PadModeHotSlice, false, 0x18},
        {io::Control::PadModeSlice, false, 0x19}, {io::Control::PadModeScale, false, 0x1a},
        {io::Control::Rec, false, 0x14},          {io::Control::Stop, false, 0x15},
        {io::Control::Play, false, 0x16},         {io::Control::Scene, false, 0x47},
        {io::Control::Pattern, false, 0x48},      {io::Control::StripHold, false, 0x0b},
        {io::Control::ExtProject, false, 0x01},   {io::Control::LiveSampling, false, 0x02},
        {io::Control::FxSelect, false, 0x03},     {io::Control::FxOn, false, 0x06},
        {io::Control::Sync, false, 0x1b},         {io::Control::Master, false, 0x1c},
        {io::Control::TempoReset, false, 0x1d},
    };
    for (const auto& m : keys)
        t[k++] = m;
    // MODE's lamps, in the order MODE steps through them
    const io::Control modes[] = {io::Control::StripModePitch, io::Control::StripModeRepeat,
                                 io::Control::StripModeUser1, io::Control::StripModeUser2};
    for (int i = 0; i < 4; ++i)
        t[k++] = {modes[i], false, static_cast<std::uint8_t>(0x07 + i)};
    for (int i = 0; i < 8; ++i) // 0x0c at the bottom of the strip
        t[k++] = {static_cast<io::Control>(static_cast<int>(io::Control::StripLed1) + i), false,
                  static_cast<std::uint8_t>(0x0c + i)};
    const io::Control bars[] = {io::Control::Bar1, io::Control::Bar2, io::Control::Bar3, io::Control::Bar4};
    for (int i = 0; i < 4; ++i)
        t[k++] = {bars[i], false, static_cast<std::uint8_t>(0x3f + i)};
    for (int i = 1; i <= 4; ++i)
        t[k++] = {io::barPositionLamp(i), false, static_cast<std::uint8_t>(0x43 + i - 1)};
    return t;
}();
} // namespace djs1000

// ---- One unit's panel ----

enum class MonoEncoding : std::uint8_t { BitAndDimFlag, TwoBit };

// One bit of the TX frame ({0, 0} = none).
struct TxBit
{
    std::uint8_t byte, mask;
};

struct PanelMap
{
    Unit unit;
    bool bigEndian;  // the 16-bit words as read (after the driver's word swap)
    int buttonBytes; // buttons in bytes 1…buttonBytes
    const ButtonGrid* buttons;
    std::span<const ButtonRole> buttonList;
    std::span<const ButtonRole> systemButtons;
    std::span<const Counter> counters;
    std::span<const AnalogWord> analog;
    int padPressureOffset, padContactOffset; // 16 words each
    int touchX, touchY;
    std::span<const LampRole> lamps;
    MonoEncoding mono;
    std::span<const MonoChannel> monoChannels;        // BitAndDimFlag
    std::span<const djs1000::TwoBitLamp> twoBitLamps; // TwoBit, by lamp id
    int txBrightnessOffset;                           // PAD/BUTTON BRIGHTNESS 1…4 (4 = normal)
    int txMarkerOffset;                               // -1 = none
    std::uint8_t txMarker;
    TxBit standby, hardMute;
    // DJS-1000: the panel keeps the outputs muted while the mute bit is set, so the bit stays set until the app's
    // audio runs (LampBus::setOutputsLive). The startup bit stays set until the panel has answered.
    bool muteUntilLive;
    TxBit startup;
    std::uint32_t intervalMs; // the link's exchange interval
    bool takeOver;            // a link left running as wanted is used as it is, and left running on close
};

inline constexpr PanelMap kSp16Map {Unit::Sp16,
                                    true,
                                    5,
                                    &kButtonGrid,
                                    kButtons,
                                    kSystemButtons,
                                    kCounters,
                                    kAnalog,
                                    kPadPressureOffset,
                                    kPadContactOffset,
                                    kTouchXOffset,
                                    kTouchYOffset,
                                    kLamps,
                                    MonoEncoding::BitAndDimFlag,
                                    kMono,
                                    {},
                                    kTxBrightnessOffset,
                                    kTxMarkerOffset,
                                    kTxMarker,
                                    {kTxStandbyOffset, 1},
                                    {kTxHardMuteOffset, 1},
                                    false,
                                    {0, 0},
                                    3,
                                    false};

inline constexpr PanelMap kDjs1000Map {Unit::Djs1000,
                                       false,
                                       6,
                                       &djs1000::kButtonGrid,
                                       djs1000::kButtons,
                                       djs1000::kSystemButtons,
                                       djs1000::kCounters,
                                       djs1000::kAnalog,
                                       djs1000::kPadPressureOffset,
                                       djs1000::kPadContactOffset,
                                       djs1000::kTouchXOffset,
                                       djs1000::kTouchYOffset,
                                       djs1000::kLamps,
                                       MonoEncoding::TwoBit,
                                       {},
                                       djs1000::kTwoBit,
                                       djs1000::kTxBrightnessOffset,
                                       -1,
                                       0,
                                       {djs1000::kTxFlagsOffset, djs1000::kFlagStandby},
                                       {djs1000::kTxFlagsOffset, djs1000::kFlagMute},
                                       true,
                                       {djs1000::kTxFlagsOffset, djs1000::kFlagStartup},
                                       2,
                                       true};

constexpr const PanelMap& panelMap(Unit u) noexcept { return u == Unit::Djs1000 ? kDjs1000Map : kSp16Map; }
} // namespace platform::imx6::panel
