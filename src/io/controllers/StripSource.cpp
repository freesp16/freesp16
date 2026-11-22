#include "io/controllers/StripSource.h"

#include "core/commands/Command.h"
#include "core/model/Project.h"

#include <cstdio>
#include <optional>

namespace io
{
namespace
{
core::ParamId idOf(const core::ParamRegistry& r, std::string_view id) noexcept
{
    return r.idOf(id).value_or(core::ParamId {});
}

bool postParam(engine::EngineBridge& b, core::ParamId id, std::uint16_t scope, float value)
{
    return ! id.valid() || b.post(engine::ParamChange {id, scope, value, 0});
}
} // namespace

bool postRibbonSettings(const core::model::RibbonSettings& r, engine::EngineBridge& b, const core::ParamRegistry& reg)
{
    bool ok = postParam(b, idOf(reg, "ribbon.pitchTop"), 0, static_cast<float>(r.pitchTopCents));
    ok = postParam(b, idOf(reg, "ribbon.pitchBottom"), 0, static_cast<float>(r.pitchBottomCents)) && ok;
    const auto target = idOf(reg, "ribbon.userTarget"), top = idOf(reg, "ribbon.userTop"),
               bottom = idOf(reg, "ribbon.userBottom");
    for (int s = 0; s < core::ribbon::kUserSets; ++s)
        for (int i = 0; i < core::ribbon::kUserItems; ++i)
        {
            const auto& it = r.user[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)];
            const auto scope = static_cast<std::uint16_t>(s * core::ribbon::kUserItems + i);
            const auto id = it.target.empty() ? std::optional<core::ParamId> {} : reg.idOf(it.target);
            // A target the registry does not know (a project from a newer build) is an empty slot.
            ok = postParam(b, target, scope, id ? static_cast<float>(id->value) : -1.0f) && ok;
            ok = postParam(b, top, scope, static_cast<float>(it.top)) && ok;
            ok = postParam(b, bottom, scope, static_cast<float>(it.bottom)) && ok;
        }
    return ok;
}

StripRouter::StripRouter(engine::EngineBridge& bridge, const core::ParamRegistry& params)
    : bridge_(bridge), params_(params), modeId_(idOf(params, "ribbon.mode")), holdId_(idOf(params, "ribbon.hold"))
{
}

void StripRouter::post(const engine::EngineMessage& m)
{
    if (bridge_.post(m))
        ++posted_;
    else
        ++refused_;
}

void StripRouter::postParam(core::ParamId id, std::uint16_t scope, float value)
{
    if (id.valid())
        post(engine::ParamChange {id, scope, value, 0});
}

void StripRouter::say(const char* what, int value, std::uint8_t source)
{
    if (! log)
        return;
    char line[160];
    if (value >= 0)
        std::snprintf(line, sizeof line, "RIBBON %s %d source=%u mode=%s hold=%d leds=0x%02x", what, value,
                      static_cast<unsigned>(source), core::ribbon::modeName(mode_), hold_ ? 1 : 0, ledMask());
    else
        std::snprintf(line, sizeof line, "RIBBON %s mode=%s hold=%d leds=0x%02x", what, core::ribbon::modeName(mode_),
                      hold_ ? 1 : 0, ledMask());
    log(line);
}

void StripRouter::handle(const ControlEvent& e)
{
    using Kind = ControlEvent::Kind;
    if (e.control == Control::StripMode && e.kind == Kind::Press)
    {
        mode_ = core::ribbon::nextMode(mode_); // RIB-001
        hold_ = latched_ = false;             // RIB-005: MODE cancels HOLD
        postParam(modeId_, 0, static_cast<float>(mode_));
        postParam(holdId_, 0, 0.0f);
        say("mode");
        return;
    }
    if (e.control == Control::StripHold && e.kind == Kind::Press)
    {
        hold_ = ! hold_;
        if (! hold_)
            latched_ = false; // `releaseParameters`
        postParam(holdId_, 0, hold_ ? 1.0f : 0.0f);
        say("hold");
        return;
    }
    if (e.control == Control::Strip)
    {
        const bool down = e.kind == Kind::Touch || e.kind == Kind::Value || e.kind == Kind::Press;
        if (! down && e.kind != Kind::Release)
            return;
        const int p = core::ribbon::clampPosition(e.value);
        const bool landed = down && ! contact_;
        if (down)
            position_ = p;
        else if (contact_ && hold_)
        {
            latched_ = true; // HOLD keeps the last value and its LED (RIB-005)
            latchedPosition_ = position_;
        }
        const bool lifted = ! down && contact_;
        contact_ = down;
        if (e.source != kSourceStrip) // the device's reader thread already sent it (UI-118)
            post(stripTrigger(down ? p : position_, down));
        if (landed)
            say("touch", p, e.source);
        else if (lifted)
            say("release", position_, e.source);
        return;
    }
    if (isPad(e.control) && e.kind == Kind::Pressure && mode_ == core::ribbon::Mode::Repeat)
        post(pressureTrigger(indexOf(e.control) - 1, e.value)); // RIB-003: the repeat volume follows the pressure
}

void StripRouter::forwardChanges(const core::ChangeSet& changes, const core::model::Project& p)
{
    if (changes.touches("ribbon"))
        if (! postRibbonSettings(p.ribbon, bridge_, params_))
            ++refused_;
}

int StripRouter::shownPosition() const noexcept
{
    return contact_ ? position_ : (hold_ && latched_ ? latchedPosition_ : -1);
}

std::uint8_t StripRouter::ledMask() const noexcept
{
    const int p = shownPosition();
    return core::ribbon::ledMask(mode_, p < 0 ? 0 : p, p >= 0);
}

void StripRouter::lamps(LampFrame& f) const noexcept
{
    const std::uint8_t mask = ledMask();
    for (int i = 0; i < core::ribbon::kLeds; ++i)
        f[static_cast<Control>(static_cast<int>(Control::StripLed1) + i)] =
            (mask >> i & 1u) != 0 ? LampState {LampMode::On, lampcolour::white, 0} : LampState {};
    const Control modes[] = {Control::StripModePitch, Control::StripModeRepeat, Control::StripModeUser1,
                             Control::StripModeUser2};
    for (int i = 0; i < core::ribbon::kModes; ++i)
        f[modes[i]] = static_cast<int>(mode_) == i ? LampState {LampMode::On, lampcolour::white, 0} : LampState {};
    f[Control::StripHold] = hold_ ? LampState {LampMode::On, lampcolour::white, 0} : LampState {};
}
} // namespace io
