#include "platform/host/display/OffscreenDisplay.h"

namespace platform::host
{
namespace
{
class OffscreenDisplay final : public DisplayDevice
{
public:
    OffscreenDisplay(int w, int h) : w_(w), h_(h) {}
    int width() const noexcept override { return w_; }
    int height() const noexcept override { return h_; }
    std::vector<std::string> rendererOrder() const override { return {"soft", "juce"}; }
    ui::RendererEnv rendererEnv() override
    {
        ui::RendererEnv env;
        env.width = w_;
        env.height = h_;
        const int w = w_, h = h_;
        env.windowPresenter = [w, h](std::string&) { return ui::makeMemoryPresenter(w, h); };
        env.nativeWindow = [](std::string_view kind, std::string& error) -> std::optional<ui::NativeWindow> {
            error = "host: no native window of kind " + std::string(kind) + " (offscreen display)";
            return std::nullopt;
        };
        env.loadGl = [](std::string_view, std::string& error) {
            error = "host: no GL driver (offscreen display)";
            return false;
        };
        env.glReport = [] { return std::string("host: no GL"); };
        return env;
    }

private:
    int w_, h_;
};
} // namespace

std::unique_ptr<DisplayDevice> makeOffscreenDisplay(int width, int height)
{
    return std::make_unique<OffscreenDisplay>(width, height);
}
} // namespace platform::host
