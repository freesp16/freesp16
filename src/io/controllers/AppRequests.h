// Requests an input source makes of the app itself: quit (the linux window's close button, the scripted
// input's `quit`) and a screenshot (the scripted input's `screenshot <file.png>`). The app's frame loop reads them once
// per frame (app/App.cpp, app/Screenshot.h); they are not control events (no screen sees them). UI/message thread;
// quit may also be requested from any thread (atomic).
#pragma once

#include <string>

namespace io
{
void requestQuit() noexcept;
bool quitRequested() noexcept;
void clearQuitRequest() noexcept; // tests

void requestScreenshot(const std::string& path);
bool takeScreenshotRequest(std::string& path); // the oldest pending request, false when none

// An app command (the scripted input's `cmd <text>`): the app hands it to the document once per frame.
void requestAppCommand(const std::string& text);
bool takeAppCommand(std::string& text); // the oldest pending command, false when none
} // namespace io
