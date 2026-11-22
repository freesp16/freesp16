#include "platform/linux/input/X11Input.h"

#include "platform/linux/input/PointerMap.h"
#include "platform/linux/input/XKeyMap.h"

#include "io/controllers/AppRequests.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>

namespace platform::linux_desktop
{
namespace
{
double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

X11InputSource::X11InputSource(engine::TriggerSink* triggers, std::shared_ptr<LinuxWindow> window)
    : window_(std::move(window)), keyboard_(triggers)
{
}

bool X11InputSource::start(std::string& info)
{
    info = "x11: keyboard (UI-120 map), mouse button 1 = LCD touch (800x480 points, window / "
         + std::to_string(window_->scale) + "), wheel = selector, Ctrl + wheel = touch strip, close = quit";
    return true;
}

void X11InputSource::stop() {}

void X11InputSource::releaseAll(double t, std::vector<io::ControlEvent>& out)
{
    for (auto& code : held_)
        if (code != 0)
        {
            keyboard_.key(code, mods_, false, false, t);
            code = 0;
        }
    if (mods_ != 0)
        keyboard_.setModifiers(mods_ = 0, t);
    if (const auto lift = strip_.lift(t))
        out.push_back(*lift);
    if (touching_)
    {
        io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen, 0, io::kSourceKeyboard, t};
        e.x = lastX_;
        e.y = lastY_;
        out.push_back(e);
        touching_ = false;
    }
    keyboard_.poll(out);
}

void X11InputSource::poll(std::vector<io::ControlEvent>& out)
{
    if (! window_->x)
        return;
    auto* dpy = static_cast<Display*>(common::x11Display(*window_->x));
    if (attached_ != dpy)
    {
        Bool supported = False;
        XkbSetDetectableAutoRepeat(dpy, True, &supported); // repeats arrive as press, press, …, release
        attached_ = dpy;
    }
    const auto win = static_cast<Window>(common::x11Drawable(*window_->x));
    const auto wmDelete = static_cast<Atom>(common::x11WmDeleteAtom(*window_->x));
    const int scale = window_->scale;
    while (XPending(dpy) > 0) // never blocks: only what already arrived
    {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.xany.window != win)
            continue;
        const double t = nowMs();
        switch (ev.type)
        {
        case KeyPress:
        case KeyRelease:
        {
            const bool down = ev.type == KeyPress;
            const unsigned long ks = XLookupKeysym(&ev.xkey, 0);
            const unsigned keycode = ev.xkey.keycode & 0xFF;
            if (const unsigned bit = modifierForKeysym(ks))
            {
                mods_ = down ? (modsForState(ev.xkey.state) | bit) : (modsForState(ev.xkey.state) & ~bit);
                keyboard_.setModifiers(mods_, t);
                if ((mods_ & host::kModCtrl) == 0)
                    if (const auto lift = strip_.lift(t)) // Ctrl up: the strip is released
                        out.push_back(*lift);
                break;
            }
            mods_ = modsForState(ev.xkey.state);
            const int code = keyCodeForKeysym(ks);
            if (code == 0)
                break;
            if (down)
            {
                const bool repeat = held_[keycode] != 0;
                held_[keycode] = code;
                keyboard_.key(code, mods_, true, repeat, t);
            }
            else if (held_[keycode] != 0)
            {
                keyboard_.key(held_[keycode], mods_, false, false, t);
                held_[keycode] = 0;
            }
            break;
        }
        case ButtonPress:
        case ButtonRelease:
        {
            const auto p = toLogicalPoint(ev.xbutton.x, ev.xbutton.y, scale);
            if (ev.xbutton.button == Button1)
            {
                const bool down = ev.type == ButtonPress;
                if (down == touching_)
                    break;
                touching_ = down;
                io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen,
                                    static_cast<std::int16_t>(down ? 1 : 0), io::kSourceKeyboard, t};
                e.x = lastX_ = p.x;
                e.y = lastY_ = p.y;
                out.push_back(e);
            }
            else if (ev.type == ButtonPress && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5))
            {
                const auto detents = static_cast<std::int16_t>(ev.xbutton.button == Button4 ? 1 : -1); // up = +1
                if ((ev.xbutton.state & ControlMask) != 0) // Ctrl + wheel = the touch strip (StripWheel.h)
                    out.push_back(strip_.wheel(detents, t));
                else
                    out.push_back({io::ControlEvent::Kind::Turn, io::Control::Selector, detents, io::kSourceKeyboard, t});
            }
            break;
        }
        case MotionNotify:
        {
            if (! touching_)
                break;
            const auto p = toLogicalPoint(ev.xmotion.x, ev.xmotion.y, scale);
            if (p.x == lastX_ && p.y == lastY_)
                break;
            io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen, 2, io::kSourceKeyboard, t};
            e.x = lastX_ = p.x;
            e.y = lastY_ = p.y;
            out.push_back(e);
            break;
        }
        case FocusOut: releaseAll(t, out); break;
        case Expose:
            if (ev.xexpose.count == 0)
                common::x11RequestFullPresent(*window_->x);
            break;
        case ClientMessage:
            if (wmDelete != 0 && static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete)
                io::requestQuit();
            break;
        case DestroyNotify: io::requestQuit(); break;
        default: break;
        }
        keyboard_.poll(out); // keep the keyboard's events in order with the mouse's
    }
}
} // namespace platform::linux_desktop
