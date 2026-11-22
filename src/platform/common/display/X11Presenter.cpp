#include "platform/common/display/X11Window.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace platform::common
{
using ui::PixelPresenter;

struct X11Window
{
    Display* dpy = nullptr;
    Window win = 0;
    GC gc = nullptr;
    int w = 0, h = 0, depth = 0;
    Visual* visual = nullptr;
    Atom wmDelete = 0;        // managed windows: WM_DELETE_WINDOW
    bool fullPresent = false; // set after an Expose: the next present sends the whole frame
    ~X11Window()
    {
        if (dpy != nullptr)
            XCloseDisplay(dpy);
    }
};

bool x11ScreenSize(int& w, int& h)
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr)
        return false;
    w = DisplayWidth(dpy, DefaultScreen(dpy));
    h = DisplayHeight(dpy, DefaultScreen(dpy));
    XCloseDisplay(dpy);
    return true;
}

namespace
{
// A managed window is mapped by the window manager: wait for its MapNotify so the first frame is not lost (an Expose
// later re-sends the whole frame anyway). Other events stay queued for the input reader.
void waitMapped(Display* dpy, Window win)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XEvent ev;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (XCheckTypedWindowEvent(dpy, win, MapNotify, &ev))
            return;
        pollfd fd {ConnectionNumber(dpy), POLLIN, 0};
        ::poll(&fd, 1, 10);
    }
}
} // namespace

std::shared_ptr<X11Window> openX11Window(bool inputOnly, std::string& error)
{
    X11WindowOptions o;
    o.inputOnly = inputOnly;
    return openX11Window(o, error);
}

std::shared_ptr<X11Window> openX11Window(const X11WindowOptions& o, std::string& error)
{
    const bool inputOnly = o.inputOnly;
    auto x = std::make_shared<X11Window>();
    x->dpy = XOpenDisplay(nullptr);
    if (x->dpy == nullptr)
    {
        error = "XOpenDisplay failed (DISPLAY not set or server unreachable)";
        return nullptr;
    }
    // A TCP display (the emulator's Xvfb; DISPLAY=host:n) must not hold small requests back: with Nagle on, a partial
    // XPutImage + XSync waits for the server's delayed ACK (~40 ms per frame). Harmless on a Unix socket (it fails).
    {
        int one = 1;
        setsockopt(ConnectionNumber(x->dpy), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    const int screen = DefaultScreen(x->dpy);
    const Window root = RootWindow(x->dpy, screen);
    x->w = DisplayWidth(x->dpy, screen);
    x->h = DisplayHeight(x->dpy, screen);
    x->depth = DefaultDepth(x->dpy, screen);
    x->visual = DefaultVisual(x->dpy, screen);
    if (o.managed)
    {
        if (o.width > 0 && o.height > 0)
            x->w = o.width, x->h = o.height;
        XSetWindowAttributes a;
        std::memset(&a, 0, sizeof a);
        a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask
                     | ExposureMask | StructureNotifyMask | FocusChangeMask;
        a.background_pixmap = None; // X never paints over a frame
        x->win = XCreateWindow(x->dpy, root, 0, 0, static_cast<unsigned>(x->w), static_cast<unsigned>(x->h), 0,
                               inputOnly ? 0 : x->depth, inputOnly ? InputOnly : InputOutput,
                               inputOnly ? CopyFromParent : x->visual,
                               CWEventMask | (inputOnly ? 0ul : static_cast<unsigned long>(CWBackPixmap)), &a);
        XSizeHints* hints = XAllocSizeHints(); // fixed size: the frame is 800 x 480 x scale, never stretched
        if (hints != nullptr)
        {
            hints->flags = PSize | PMinSize | PMaxSize;
            hints->width = hints->min_width = hints->max_width = x->w;
            hints->height = hints->min_height = hints->max_height = x->h;
            XSetWMNormalHints(x->dpy, x->win, hints);
            XFree(hints);
        }
        XStoreName(x->dpy, x->win, o.title.c_str());
        const Atom utf8 = XInternAtom(x->dpy, "UTF8_STRING", False);
        XChangeProperty(x->dpy, x->win, XInternAtom(x->dpy, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(o.title.c_str()), static_cast<int>(o.title.size()));
        x->wmDelete = XInternAtom(x->dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(x->dpy, x->win, &x->wmDelete, 1);
        XMapWindow(x->dpy, x->win);
        if (! inputOnly)
            x->gc = XCreateGC(x->dpy, x->win, 0, nullptr);
        XSync(x->dpy, False);
        waitMapped(x->dpy, x->win);
        return x;
    }

    XSetWindowAttributes a;
    std::memset(&a, 0, sizeof a);
    a.override_redirect = True; // no window manager on the device
    a.event_mask = ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
    a.background_pixmap = None; // X never paints over a frame
    if (inputOnly)
        x->win = XCreateWindow(x->dpy, root, 0, 0, static_cast<unsigned>(x->w), static_cast<unsigned>(x->h), 0, 0,
                               InputOnly, CopyFromParent, CWOverrideRedirect | CWEventMask, &a);
    else
        x->win = XCreateWindow(x->dpy, root, 0, 0, static_cast<unsigned>(x->w), static_cast<unsigned>(x->h), 0,
                               x->depth, InputOutput, x->visual, CWOverrideRedirect | CWEventMask | CWBackPixmap, &a);

    const Pixmap empty = XCreatePixmap(x->dpy, root, 1, 1, 1);
    XColor black;
    std::memset(&black, 0, sizeof black);
    const Cursor none = XCreatePixmapCursor(x->dpy, empty, empty, &black, &black, 0, 0);
    XDefineCursor(x->dpy, x->win, none);
    XFreePixmap(x->dpy, empty);
    XMapRaised(x->dpy, x->win);
    if (! inputOnly)
        x->gc = XCreateGC(x->dpy, x->win, 0, nullptr);
    XSync(x->dpy, False);
    return x;
}

void x11Size(const X11Window& x, int& w, int& h)
{
    w = x.w;
    h = x.h;
}

// Native window handles for the GLES backend (EGL x11 flavour).
void* x11Display(const X11Window& x)
{
    return x.dpy;
}
unsigned long x11Drawable(const X11Window& x)
{
    return x.win;
}
unsigned long x11WmDeleteAtom(const X11Window& x)
{
    return x.wmDelete;
}
void x11RequestFullPresent(X11Window& x) noexcept
{
    x.fullPresent = true;
}

namespace
{
class X11Presenter final : public PixelPresenter
{
public:
    explicit X11Presenter(std::shared_ptr<X11Window> x) : x_(std::move(x)) {}
    ~X11Presenter() override
    {
        if (image_ != nullptr)
        {
            // The pixels are never Xlib's: buffer_ (zero-copy) or imageWords_ (RGB565, the device's 16-bit X server)
            // own them. Left set, XDestroyImage would free() imageWords_ and its destructor would free it again (a
            // SIGSEGV on the device: the 750 KB block is mmapped, so the second free reads an unmapped header).
            image_->data = nullptr;
            XDestroyImage(image_);
        }
    }

    bool init(std::string& error)
    {
        buffer_.assign(static_cast<std::size_t>(x_->w) * static_cast<std::size_t>(x_->h), 0u);
        const int bpp = x_->depth > 16 ? 32 : 16;
        imageWords_.resize(static_cast<std::size_t>(x_->w) * static_cast<std::size_t>(x_->h) *
                               static_cast<std::size_t>(bpp / 8) / 4 +
                           1);
        imageBytes_ = reinterpret_cast<char*>(imageWords_.data());
        image_ = XCreateImage(x_->dpy, x_->visual, static_cast<unsigned>(x_->depth), ZPixmap, 0, imageBytes_,
                              static_cast<unsigned>(x_->w), static_cast<unsigned>(x_->h), bpp, 0);
        if (image_ == nullptr)
        {
            error = "XCreateImage failed";
            return false;
        }
        const std::uint16_t probe = 1;
        const bool hostLsb = *reinterpret_cast<const std::uint8_t*>(&probe) == 1;
        direct_ = image_->bits_per_pixel == 32 && x_->visual->red_mask == 0xFF0000 &&
                  x_->visual->green_mask == 0xFF00 && x_->visual->blue_mask == 0xFF &&
                  (image_->byte_order == LSBFirst) == hostLsb && image_->bytes_per_line == x_->w * 4;
        rgb565_ = image_->bits_per_pixel == 16 && x_->visual->red_mask == 0xF800 && x_->visual->green_mask == 0x07E0 &&
                  x_->visual->blue_mask == 0x001F && (image_->byte_order == LSBFirst) == hostLsb;
        if (direct_)
            image_->data = reinterpret_cast<char*>(buffer_.data()); // the rasteriser draws straight into the XImage
        if (! direct_ && ! rgb565_)
        {
            error = "unsupported X visual (depth " + std::to_string(x_->depth) + ")";
            return false;
        }
        return true;
    }

    int width() const noexcept override { return x_->w; }
    int height() const noexcept override { return x_->h; }
    std::uint32_t* pixels() noexcept override { return buffer_.data(); }
    int stridePixels() const noexcept override { return x_->w; }

    bool present() override
    {
        const ui::IRect all{0, 0, x_->w, x_->h};
        return presentRects({&all, 1});
    }

    // Damage: only the changed rects are converted (RGB565) and sent (one XPutImage each), then one XSync.
    bool presentRects(std::span<const ui::IRect> rects) override
    {
        if (x_->fullPresent) // an Expose (managed window) lost the window's content: send the whole frame
        {
            x_->fullPresent = false;
            const ui::IRect all{0, 0, x_->w, x_->h};
            return presentRects({&all, 1});
        }
        for (const auto& r0 : rects)
        {
            const ui::IRect r = ui::intersect(r0, {0, 0, x_->w, x_->h});
            if (r.empty())
                continue;
            if (rgb565_)
            {
                auto* dst = reinterpret_cast<std::uint16_t*>(imageWords_.data());
                for (int y = r.y0; y < r.y1; ++y)
                    for (int x = r.x0; x < r.x1; ++x)
                    {
                        const std::size_t i =
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(x_->w) + static_cast<std::size_t>(x);
                        const std::uint32_t p = buffer_[i];
                        dst[i] =
                            static_cast<std::uint16_t>(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
                    }
            }
            XPutImage(x_->dpy, x_->win, x_->gc, image_, r.x0, r.y0, r.x0, r.y0, static_cast<unsigned>(r.x1 - r.x0),
                      static_cast<unsigned>(r.y1 - r.y0));
        }
        XSync(x_->dpy, False);
        return true;
    }

    std::string describe() const override
    {
        return "x11 " + std::to_string(x_->w) + "x" + std::to_string(x_->h) + " depth " + std::to_string(x_->depth) +
               (direct_ ? " (XImage zero-copy, XPutImage of the damaged rects + XSync)"
                        : " (RGB565 convert, XPutImage of the damaged rects + XSync)");
    }

private:
    std::shared_ptr<X11Window> x_;
    std::vector<std::uint32_t> buffer_;
    std::vector<std::uint32_t> imageWords_; // 32-bit aligned backing store of the XImage
    char* imageBytes_ = nullptr;
    XImage* image_ = nullptr;
    bool direct_ = false, rgb565_ = false;
};
} // namespace

std::unique_ptr<PixelPresenter> makeX11Presenter(std::shared_ptr<X11Window> x, std::string& error)
{
    auto p = std::make_unique<X11Presenter>(std::move(x));
    if (! p->init(error))
        return nullptr;
    return p;
}
} // namespace platform::common
