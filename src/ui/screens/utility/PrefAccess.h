// The preferences behind the shell's ParamAccess (SET-010, UI-325): the composition root wraps the
// shell's values in a PreferenceAccess, so every edit of a preference ParamSpec — from UTILITY, BROWSE SETTING, the
// MIXER, RENDER, a script — is saved (services::settings::persists: Global scope, not session./internal., plus the
// machine settings), whoever made it. applyPreferences() pushes the stored values into a ParamAccess at start-up and
// after an import, so the engine and the screens start from the user's values. Message thread.
#pragma once

#include "core/params/ParamRegistry.h"
#include "services/settings/Preferences.h"
#include "ui/shell/ParamAccess.h"

namespace ui::utility
{
class PreferenceAccess final : public ParamAccess
{
public:
    PreferenceAccess(ParamAccess& inner, services::settings::Preferences& prefs) noexcept
        : inner_(inner), prefs_(prefs)
    {
    }
    float get(const core::ParamSpec& s, std::uint16_t scope) const override { return inner_.get(s, scope); }
    void set(const core::ParamSpec& s, std::uint16_t scope, float v) override;
    bool lockOf(const core::ParamSpec& s, std::uint16_t scope, int step, float& v) const override
    {
        return inner_.lockOf(s, scope, step, v);
    }
    void setLock(const core::ParamSpec& s, std::uint16_t scope, int step, float v) override
    {
        inner_.setLock(s, scope, step, v);
    }
    ParamAccess& inner() noexcept { return inner_; }

private:
    ParamAccess& inner_;
    services::settings::Preferences& prefs_;
};

// Sets every stored ParamSpec preference (key "<storage key>[#scope]") in `values`; returns how many.
int applyPreferences(const services::settings::Preferences&, ParamAccess& values,
                     const core::ParamRegistry& = core::builtinParams());
} // namespace ui::utility
