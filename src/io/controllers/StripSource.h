// The touch strip on the message thread (spec 02-features/touch-strip.md RIB-001…006/009/011, 03-ui/
// control-surface.md UI-110 strip events, UI-156/157 lamps). The device's reader (platform/imx6/strip/StripSource, the
// "cdc-strip" ControlSource) and every other strip — the linux window's Ctrl + wheel, the scripted input's
// `strip`, a controller — produce Control::Strip events; this side turns them, the MODE and HOLD keys and the pads'
// pressure into the engine's ribbon messages and the strip's lamps:
//
//   MODE (strip.mode) press   PITCH → REPEAT → USER1 → USER2 → PITCH (RIB-001); turns HOLD off (RIB-005); posts
//                             ribbon.mode and ribbon.hold
//   HOLD (strip.hold) press   toggles HOLD (RIB-005); posts ribbon.hold; off = the values return to base
//   strip Touch/Value/Release the LED position; events from any source but the device strip (kSourceStrip, whose
//                             reader thread already sent them to the engine, UI-118) are posted as the strip stream
//                             (stripTrigger, core/util/ribbon/Ribbon.h) through the EngineBridge
//   pad Pressure              in REPEAT mode, posted as pressureTrigger (the repeat volume follows it, RIB-003)
//   forwardChanges / postRibbonSettings   the project's RibbonSettings (ChangeSet path "ribbon", RIB-009) →
//                             ribbon.pitchTop/Bottom, ribbon.userTarget/Top/Bottom (scope = set × 8 + item)
//   lamps(frame)              the 8 strip LEDs (core::ribbon::ledMask: up to the segment in PITCH / USER, the rate
//                             zone in REPEAT; the held position while HOLD keeps a value), the MODE indicators one-hot
//                             and HOLD (UI-156/157), refining the LampModel's frame before io::apply
// The engine side is engine/ribbon/RibbonEngine.h. Message thread only; allocation-free after construction.
//
// (The device's strip reader lives in platform/imx6/strip/StripSource; this file keeps the device-independent part of
// the strip.)
#pragma once

#include "core/params/ParamRegistry.h"
#include "core/util/ribbon/Ribbon.h"
#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/LampRules.h"

#include <cstdint>
#include <functional>
#include <string>

namespace core
{
class ChangeSet;
}
namespace core::model
{
struct Project;
struct RibbonSettings;
} // namespace core::model

namespace io
{
// The strip stream as TriggerNow (core/util/ribbon/Ribbon.h): from the device's reader thread into the TriggerSink,
// or from the message thread through EngineBridge::post.
constexpr engine::TriggerNow stripTrigger(int position, bool contact) noexcept
{
    return {0, static_cast<std::uint8_t>(core::ribbon::clampPosition(position)), static_cast<std::uint8_t>(contact ? 1 : 0),
            core::ribbon::kStripPad};
}
constexpr engine::TriggerNow pressureTrigger(int pad0, int pressure) noexcept
{
    return {static_cast<std::uint8_t>(pad0 & 15),
            static_cast<std::uint8_t>(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure)), 0, core::ribbon::kPressurePad};
}

// The project's ribbon settings as engine messages (a project open, the offline render). False = a post was refused.
bool postRibbonSettings(const core::model::RibbonSettings&, engine::EngineBridge&, const core::ParamRegistry&);

class StripRouter
{
public:
    StripRouter(engine::EngineBridge&, const core::ParamRegistry&);

    void handle(const ControlEvent&); // never consumes: the screens see the keys and the strip too
    void forwardChanges(const core::ChangeSet&, const core::model::Project&);
    void lamps(LampFrame&) const noexcept;

    core::ribbon::Mode mode() const noexcept { return mode_; }
    bool hold() const noexcept { return hold_; }
    bool contact() const noexcept { return contact_; }
    int position() const noexcept { return position_; }
    int shownPosition() const noexcept; // touched, or held under HOLD; −1 = dark
    std::uint8_t ledMask() const noexcept;
    std::uint64_t posted() const noexcept { return posted_; }
    std::uint64_t refused() const noexcept { return refused_; }

    std::function<void(const std::string&)> log; // "RIBBON …" lines (mode / hold changes, touch / release)

private:
    void post(const engine::EngineMessage&);
    void postParam(core::ParamId, std::uint16_t scope, float value);
    void say(const char* what, int value = -1, std::uint8_t source = 0);

    engine::EngineBridge& bridge_;
    const core::ParamRegistry& params_;
    core::ParamId modeId_ {}, holdId_ {};
    core::ribbon::Mode mode_ = core::ribbon::Mode::Pitch;
    bool hold_ = false, contact_ = false, latched_ = false;
    int position_ = core::ribbon::kCentre, latchedPosition_ = core::ribbon::kCentre;
    std::uint64_t posted_ = 0, refused_ = 0;
};
} // namespace io
