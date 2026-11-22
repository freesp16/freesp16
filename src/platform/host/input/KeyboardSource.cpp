// The keyboard source (see KeyboardSource.h).
#include "platform/host/input/KeyboardSource.h"

#include <algorithm>
#include <chrono>

namespace platform::host
{
namespace
{
double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool KeyboardSource::start(std::string& info)
{
    info = "keyboard: UI-120 default map (attach to a window as juce::KeyListener)";
    return true;
}

void KeyboardSource::poll(std::vector<io::ControlEvent>& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

bool KeyboardSource::key(int code, unsigned mods, bool down, bool repeat, double t)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.key(code, mods, down, repeat, t, triggers_, pending_);
}

void KeyboardSource::setModifiers(unsigned mods, double t)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_.modifiers(mods, t, pending_);
}

void KeyboardSource::setTextMode(bool on)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_.setTextMode(on);
}

void KeyboardSource::setFixedVelocity(int v)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_.setFixedVelocity(v);
}

int KeyboardSource::codeFor(const juce::KeyPress& k) noexcept
{
    const int c = k.getKeyCode();
    for (int i = 0; i < 12; ++i)
        if (c == juce::KeyPress::F1Key + i)
            return kKeyF1 + i;
    if (c == juce::KeyPress::leftKey)
        return kKeyLeft;
    if (c == juce::KeyPress::rightKey)
        return kKeyRight;
    if (c == juce::KeyPress::upKey)
        return kKeyUp;
    if (c == juce::KeyPress::downKey)
        return kKeyDown;
    if (c == juce::KeyPress::returnKey)
        return kKeyReturn;
    if (c == juce::KeyPress::escapeKey)
        return kKeyEscape;
    if (c == juce::KeyPress::homeKey)
        return kKeyHome;
    if (c == juce::KeyPress::deleteKey)
        return kKeyDelete;
    if (c == juce::KeyPress::backspaceKey)
        return kKeyBackspace;
    if (c == juce::KeyPress::spaceKey)
        return ' ';
    return c > 0 && c < 128 ? c : 0;
}

unsigned KeyboardSource::modsFor(const juce::ModifierKeys& m) noexcept
{
    return (m.isShiftDown() ? kModShift : 0u) | (m.isCtrlDown() ? kModCtrl : 0u) | (m.isAltDown() ? kModAlt : 0u)
         | (m.isCommandDown() && ! m.isCtrlDown() ? kModCmd : 0u);
}

bool KeyboardSource::keyPressed(const juce::KeyPress& k, juce::Component*)
{
    const int code = codeFor(k);
    if (code == 0)
        return false;
    const auto held = [&](const Down& d) { return d.juceCode == k.getKeyCode(); };
    const bool repeat = std::any_of(down_.begin(), down_.end(), held);
    if (! repeat)
        down_.push_back({k.getKeyCode(), code});
    return key(code, modsFor(k.getModifiers()), true, repeat, nowMs());
}

bool KeyboardSource::keyStateChanged(bool, juce::Component*)
{
    const double t = nowMs();
    bool used = false;
    for (auto it = down_.begin(); it != down_.end();)
        if (! juce::KeyPress::isKeyCurrentlyDown(it->juceCode))
        {
            used = key(it->code, modsFor(juce::ModifierKeys::getCurrentModifiers()), false, false, t) || used;
            it = down_.erase(it);
        }
        else
            ++it;
    setModifiers(modsFor(juce::ModifierKeys::getCurrentModifiers()), t);
    return used;
}
} // namespace platform::host
