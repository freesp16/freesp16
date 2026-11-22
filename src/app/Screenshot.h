// Diagnostics every platform shares: the scripted input and screenshots of the running app.
//   input_script=<file>                 the scripted input source (io/controllers/ScriptedInput.h)
//   screenshot=<file.png>               write the frame after `screenshot_frame` (default 60) to a PNG
//   screenshot_frame=<N>
// plus the scripted `screenshot <file.png>` command (io::requestScreenshot). Settings come from the platform's
// settings file; the environment <PREFIX>_<KEY> wins (as for every app setting).
// A screenshot is Renderer::readPixels → PNG (JUCE PNGImageFormat) and the log line `SCREENSHOT <path> <w>x<h>`.
#pragma once

#include "platform/Platform.h"
#include "ui/render/Renderer.h"

#include <functional>
#include <string>
#include <vector>

namespace app
{
// Fills the diagnostic keys (file value, environment override) and hands input_script to the scripted source.
// Call before the control sources are created.
void configureDiagnostics(platform::Settings&);

class Screenshot
{
public:
    Screenshot(const platform::Settings&, std::function<void(const std::string&)> log);
    // After frame `frame` (1-based) was rendered: the configured shot and any requested ones.
    void afterFrame(long frame, ui::Renderer&);

    // 0x00RRGGBB pixels (Renderer::readPixels) → an RGB PNG. False + error when the file cannot be written.
    static bool writePng(const std::string& path, const std::vector<std::uint32_t>& pixels, int width, int height,
                         std::string& error);

private:
    void take(const std::string& path, ui::Renderer&);

    std::string path_;
    long frame_ = 60;
    std::function<void(const std::string&)> log_;
};
} // namespace app
