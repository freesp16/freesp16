#include "platform/linux/display/LinuxDisplay.h"

#include "platform/common/display/GlBinding.h"

#include <cstdlib>

namespace platform::linux_desktop
{
std::shared_ptr<common::X11Window> LinuxWindow::open(std::string& error)
{
    if (! x)
    {
        common::X11WindowOptions o;
        o.managed = true;
        o.width = 800 * scale;
        o.height = 480 * scale;
        o.title = title;
        x = common::openX11Window(o, error);
    }
    return x;
}

namespace
{
class LinuxDisplay final : public DisplayDevice
{
public:
    explicit LinuxDisplay(std::shared_ptr<LinuxWindow> window) : window_(std::move(window)) {}

    int width() const noexcept override { return 800 * window_->scale; }
    int height() const noexcept override { return 480 * window_->scale; }
    bool sizeKnown() const noexcept override { return true; } // the window's size is ours, not the screen's

    std::vector<std::string> rendererOrder() const override
    {
        std::vector<std::string> order {"soft"};
        // gl-x11 exists where gldispatch does: the app on a Linux host with EGL/GLES2 headers links both the GLES2
        // renderer library and GlDispatch.cpp (app/CMakeLists.txt); the test executables link neither.
        if (common::glBinding().load != nullptr)
            order.push_back("gl-x11");
        return order;
    }

    ui::RendererEnv rendererEnv() override
    {
        ui::RendererEnv env;
        env.width = width();
        env.height = height();
        env.loadGl = [](std::string_view flavour, std::string& error) {
            if (flavour != "x11")
            {
                error = "linux: no GL flavour '" + std::string(flavour) + "' (only x11: EGL on the window)";
                return false;
            }
            const auto& gl = common::glBinding();
            if (gl.load == nullptr)
                return error = "linux: no GL dispatch in this executable", false;
            setenv("EGL_PLATFORM", "x11", 0); // Mesa: the native display is an X11 Display* (a user value wins)
            return gl.load("mesa", gldispatch::kMesa, error);
        };
        env.glReport = [] {
            const auto& gl = common::glBinding();
            return gl.report != nullptr ? gl.report() : std::string("no gldispatch");
        };
        auto window = window_;
        env.nativeWindow = [window](std::string_view kind, std::string& error) -> std::optional<ui::NativeWindow> {
            if (kind != "x11")
                return error = "linux: no native window of kind " + std::string(kind), std::nullopt;
            auto x = window->open(error);
            if (! x)
                return std::nullopt;
            ui::NativeWindow nw;
            nw.display = common::x11Display(*x);
            nw.window = common::x11Drawable(*x);
            common::x11Size(*x, nw.width, nw.height);
            nw.info = "x11 window " + std::to_string(nw.width) + "x" + std::to_string(nw.height);
            nw.owner = x;
            return nw;
        };
        env.windowPresenter = [window](std::string& error) -> std::unique_ptr<ui::PixelPresenter> {
            auto x = window->open(error);
            if (! x)
                return nullptr;
            return common::makeX11Presenter(x, error);
        };
        return env;
    }

private:
    std::shared_ptr<LinuxWindow> window_;
};
} // namespace

std::unique_ptr<DisplayDevice> makeLinuxDisplay(std::shared_ptr<LinuxWindow> window)
{
    return std::make_unique<LinuxDisplay>(std::move(window));
}
} // namespace platform::linux_desktop
