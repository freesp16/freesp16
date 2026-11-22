#include "tests/fakes/FakePlatform.h"

#include "ui/render/Renderer.h"

#include <filesystem>

namespace fakes
{
namespace
{
class FakeDisplay final : public platform::DisplayDevice
{
public:
    int width() const noexcept override { return 800; }
    int height() const noexcept override { return 480; }
    std::vector<std::string> rendererOrder() const override { return {"soft"}; }
    ui::RendererEnv rendererEnv() override
    {
        ui::RendererEnv env;
        env.windowPresenter = [](std::string&) { return ui::makeMemoryPresenter(800, 480); };
        return env;
    }
};
} // namespace

platform::Paths FakePlatform::paths() const
{
    const auto base = (std::filesystem::temp_directory_path() / "fake-platform").string();
    return {base + "/data", base + "/settings.conf", base + "/cache", {}};
}

std::vector<std::unique_ptr<platform::AudioDevice>> FakePlatform::audioDevices()
{
    std::vector<std::unique_ptr<platform::AudioDevice>> devices;
    devices.push_back(std::make_unique<FakeAudioDevice>());
    return devices;
}

std::vector<std::unique_ptr<io::ControlSource>> FakePlatform::controlSources(const platform::ControlEnv& env)
{
    std::vector<std::unique_ptr<io::ControlSource>> sources;
    sources.push_back(std::make_unique<FakeControlSource>(env.triggers));
    return sources;
}

std::unique_ptr<platform::DisplayDevice> FakePlatform::display(std::string&) { return std::make_unique<FakeDisplay>(); }

std::unique_ptr<platform::Platform> makeFakePlatform() { return std::make_unique<FakePlatform>(); }
} // namespace fakes
