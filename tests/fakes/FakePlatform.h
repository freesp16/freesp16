// FakePlatform: a platform::Platform made of fakes — FakeAudioDevice (offline driver), FakeControlSource, FakeLampSink,
// an offscreen display — for composition and contract tests. Not registered by default: a test calls
// platform::registerPlatform("fake", &fakes::makeFakePlatform) when it wants select() to see it.
#pragma once

#include "platform/Platform.h"

#include "tests/fakes/FakeAudioDevice.h"
#include "tests/fakes/FakeControlSource.h"

namespace fakes
{
class FakePlatform final : public platform::Platform
{
public:
    std::string_view id() const noexcept override { return "fake"; }
    std::string describe() const override { return "fake platform (offline driver, scripted controls, offscreen)"; }
    bool detect() const override { return detected; }
    platform::Paths paths() const override;
    std::vector<std::unique_ptr<platform::AudioDevice>> audioDevices() override;
    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const platform::ControlEnv&) override;
    std::unique_ptr<io::LampSink> lamps() override { return std::make_unique<FakeLampSink>(); }
    std::unique_ptr<platform::DisplayDevice> display(std::string& error) override;
    void configure(const platform::Settings& s) override { configured = s; }

    static inline bool detected = false; // what detect() answers (all instances)
    platform::Settings configured;
};

std::unique_ptr<platform::Platform> makeFakePlatform();
} // namespace fakes
