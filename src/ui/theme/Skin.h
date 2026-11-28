#pragma once

namespace ui::theme
{
// Values 0 and 1 are the original saved DARK and LIGHT preferences.
enum class Skin { Dark, Light, Classic };

inline Skin skinFromPreference(float value) noexcept
{
    if (value == 2)
        return Skin::Classic;
    return value == 1 ? Skin::Light : Skin::Dark;
}
} // namespace ui::theme
