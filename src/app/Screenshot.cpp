#include "app/Screenshot.h"

#include "ProjectIdentity.h"
#include "io/controllers/AppRequests.h"
#include "io/controllers/ScriptedInput.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cstdlib>

namespace app
{
namespace
{
std::string envKey(const std::string& key)
{
    std::string env = std::string(identity::envPrefix) + "_" + key;
    std::transform(env.begin(), env.end(), env.begin(), ::toupper);
    return env;
}
} // namespace

void configureDiagnostics(platform::Settings& settings)
{
    for (const char* key : {"input_script", "screenshot", "screenshot_frame"})
    {
        if (const char* v = std::getenv(envKey(key).c_str()))
            settings[key] = v;
        else
            settings.try_emplace(key, std::string(key) == "screenshot_frame" ? "60" : "");
    }
    io::setInputScriptPath(settings["input_script"]);
}

Screenshot::Screenshot(const platform::Settings& settings, std::function<void(const std::string&)> log)
    : log_(std::move(log))
{
    if (auto it = settings.find("screenshot"); it != settings.end())
        path_ = it->second;
    if (auto it = settings.find("screenshot_frame"); it != settings.end() && std::atol(it->second.c_str()) > 0)
        frame_ = std::atol(it->second.c_str());
}

void Screenshot::afterFrame(long frame, ui::Renderer& renderer)
{
    if (! path_.empty() && frame == frame_)
        take(path_, renderer);
    std::string requested;
    while (io::takeScreenshotRequest(requested))
        take(requested, renderer);
}

void Screenshot::take(const std::string& path, ui::Renderer& renderer)
{
    std::vector<std::uint32_t> pixels;
    std::string error;
    const int w = renderer.width(), h = renderer.height();
    if (! renderer.readPixels(pixels) || pixels.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h))
        error = "readPixels failed";
    else if (writePng(path, pixels, w, h, error))
    {
        log_("SCREENSHOT " + path + " " + std::to_string(w) + "x" + std::to_string(h));
        return;
    }
    log_("SCREENSHOT FAILED " + path + ": " + error);
}

bool Screenshot::writePng(const std::string& path, const std::vector<std::uint32_t>& pixels, int w, int h,
                          std::string& error)
{
    juce::Image image(juce::Image::RGB, w, h, false, juce::SoftwareImageType());
    {
        juce::Image::BitmapData data(image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                data.setPixelColour(x, y, juce::Colour(0xFF000000u | pixels[i]));
            }
    }
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(path));
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (! out.openedOk())
        return error = "cannot create " + path, false;
    juce::PNGImageFormat png;
    if (! png.writeImageToStream(image, out))
        return error = "PNG encoding failed", false;
    out.flush();
    return true;
}
} // namespace app
