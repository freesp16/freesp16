#include "io/controllers/profiles/KeyRemap.h"

#include <algorithm>

namespace io::controllers
{
namespace
{
using C = Control;
constexpr KeyBinding kDefaults[] = {
    {'z', 0, C::Pad1},   {'x', 0, C::Pad2},   {'c', 0, C::Pad3},   {'v', 0, C::Pad4},   {'a', 0, C::Pad5},
    {'s', 0, C::Pad6},   {'d', 0, C::Pad7},   {'f', 0, C::Pad8},   {'q', 0, C::Pad9},   {'w', 0, C::Pad10},
    {'e', 0, C::Pad11},  {'r', 0, C::Pad12},  {'1', 0, C::Pad13},  {'2', 0, C::Pad14},  {'3', 0, C::Pad15},
    {'4', 0, C::Pad16},  {'5', 0, C::Step1},  {'6', 0, C::Step2},  {'7', 0, C::Step3},  {'8', 0, C::Step4},
    {'9', 0, C::Step5},  {'0', 0, C::Step6},  {'-', 0, C::Step7},  {'=', 0, C::Step8},  {'t', 0, C::Step9},
    {'y', 0, C::Step10}, {'u', 0, C::Step11}, {'i', 0, C::Step12}, {'o', 0, C::Step13}, {'p', 0, C::Step14},
    {'[', 0, C::Step15}, {']', 0, C::Step16},
    {keys::f1, 0, C::Bar1}, {keys::f1 + 1, 0, C::Bar2}, {keys::f1 + 2, 0, C::Bar3}, {keys::f1 + 3, 0, C::Bar4},
    {keys::f1 + 4, 0, C::PadModeTrack}, {keys::f1 + 5, 0, C::PadModeMute}, {keys::f1 + 6, 0, C::PadModeSlice},
    {keys::f1 + 7, 0, C::PadModeScale}, {keys::f1 + 8, 0, C::Scene}, {keys::f1 + 9, 0, C::Pattern},
    {keys::f1 + 10, 0, C::StripMode}, {keys::f1 + 11, 0, C::StripHold},
    {' ', 0, C::Play},   {'b', 0, C::Stop},   {'g', 0, C::Rec},
    {keys::ret, 0, C::SelectorPush}, {keys::escape, 0, C::Back}, {'`', 0, C::Home}, {keys::home, 0, C::Home},
    {keys::del, 0, C::ExtClear}, {keys::backspace, 0, C::ExtClear},
    {'n', 0, C::ExtTap}, {'m', 0, C::ExtCue}, {'j', 0, C::ExtErase}, {'k', 0, C::ExtAccent},
    {';', 0, C::ExtNudgeDown}, {'\'', 0, C::ExtNudgeUp}, // our NUDGE keys
    {'c', 1, C::ExtCopy}, {'v', 1, C::ExtPaste}, {'z', 1, C::ExtUndo}, {'z', 2, C::ExtRedo},
    {'1', 1, C::ExtMixer}, {'2', 1, C::ExtBrowse}, {'3', 1, C::ExtUtility}, {'4', 1, C::ExtProject},
};
} // namespace

std::span<const KeyBinding> defaultKeyBindings() noexcept
{
    return kDefaults;
}

int keyClass(unsigned mods) noexcept
{
    if ((mods & (keys::modCtrl | keys::modAlt)) != 0)
        return -1;
    if ((mods & keys::modCmd) != 0)
        return (mods & keys::modShift) != 0 ? 2 : 1;
    return 0;
}

std::string keyName(int code, int cls)
{
    if (code == 0)
        return "\xE2\x80\x94"; // —
    std::string prefix = cls == 2 ? "\xE2\x87\xA7\xE2\x8C\x98" : cls == 1 ? "\xE2\x8C\x98" : ""; // ⇧⌘ / ⌘
    if (code >= keys::f1 && code < keys::f1 + 12)
        return prefix + "F" + std::to_string(code - keys::f1 + 1);
    switch (code)
    {
    case ' ': return prefix + "SPACE";
    case keys::ret: return prefix + "RETURN";
    case keys::escape: return prefix + "ESC";
    case keys::home: return prefix + "HOME";
    case keys::del: return prefix + "DEL";
    case keys::backspace: return prefix + "BKSP";
    case keys::left: return prefix + "\xE2\x86\x90";
    case keys::right: return prefix + "\xE2\x86\x92";
    case keys::up: return prefix + "\xE2\x86\x91";
    case keys::down: return prefix + "\xE2\x86\x93";
    default: break;
    }
    if (code > 32 && code < 127)
    {
        const char c = static_cast<char>(code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code);
        return prefix + std::string(1, c);
    }
    return prefix + "?";
}

bool KeyRemap::remappable(int code, int cls) const noexcept
{
    if (code <= 0 || cls < 0 || cls > 2)
        return false;
    if (code == keys::left || code == keys::right || code == keys::up || code == keys::down)
        return false;
    if (cls == 0 && (code == ',' || code == '.' || code == '\\'))
        return false;
    return true;
}

Control KeyRemap::resolve(int code, int cls, Control dflt) const noexcept
{
    for (const auto& o : overrides_)
        if (o.code == code && o.cls == cls)
            return o.control;
    if (dflt != Control::Count)
        for (const auto& o : overrides_)
            if (o.control == dflt)
                return Control::Count; // the control moved to another key
    return dflt;
}

KeyBinding KeyRemap::keyFor(Control c) const noexcept
{
    for (const auto& o : overrides_)
        if (o.control == c)
            return o;
    for (const auto& d : kDefaults)
        if (d.control == c)
        {
            // Free unless an override took the key.
            bool taken = false;
            for (const auto& o : overrides_)
                taken = taken || (o.code == d.code && o.cls == d.cls);
            if (! taken)
                return d;
        }
    return {0, 0, c};
}

void KeyRemap::assign(Control c, int code, int cls)
{
    if (! remappable(code, cls))
        return;
    overrides_.erase(std::remove_if(overrides_.begin(), overrides_.end(),
                                    [&](const KeyBinding& o) { return o.control == c || (o.code == code && o.cls == cls); }),
                     overrides_.end());
    // Assigning a control its own default key is the same as no override.
    bool isDefault = false;
    for (const auto& d : kDefaults)
        isDefault = isDefault || (d.control == c && d.code == code && d.cls == cls);
    if (! isDefault)
        overrides_.push_back({code, static_cast<std::uint8_t>(cls), c});
    ++version_;
}

void KeyRemap::reset(Control c)
{
    overrides_.erase(std::remove_if(overrides_.begin(), overrides_.end(), [&](const KeyBinding& o) { return o.control == c; }),
                     overrides_.end());
    ++version_;
}

void KeyRemap::resetAll()
{
    overrides_.clear();
    ++version_;
}

void KeyRemap::setOverrides(std::vector<KeyBinding> o)
{
    overrides_.clear();
    for (const auto& b : o)
        if (remappable(b.code, b.cls) && b.control != Control::Count)
            assign(b.control, b.code, b.cls);
    ++version_;
}

bool KeyRemap::takeCapture(KeyBinding& out, bool& cancelled) noexcept
{
    if (! ready_)
        return false;
    ready_ = false;
    out = captured_;
    cancelled = cancelled_;
    return true;
}

bool KeyRemap::offer(int code, unsigned mods) noexcept
{
    if (code == '\\' && (mods & keys::modCtrl) != 0)
    {
        ++panelToggles_;
        return true;
    }
    if (! armed_)
        return false;
    armed_ = false;
    ready_ = true;
    const int cls = keyClass(mods);
    cancelled_ = code == keys::escape && cls == 0;
    captured_ = {};
    if (! cancelled_ && remappable(code, cls))
        captured_ = {code, static_cast<std::uint8_t>(cls), Control::Count};
    else
        cancelled_ = true;
    return true;
}

KeyRemap& keyRemap() noexcept
{
    static KeyRemap remap;
    return remap;
}
} // namespace io::controllers
