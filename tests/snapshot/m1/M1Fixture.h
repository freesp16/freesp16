// The sample-playback screens through the real shell with the built-in registries (screens, widgets, parameters),
// the merged parameter catalogues and the mockups' demo data.
#pragma once

#include "tests/snapshot/frame/Fixture.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/widgets/Meter.h"

namespace m1test
{
struct M1Fixture
{
    std::unique_ptr<ui::Shell> shell;
    double now = 10000;
    ui::DrawList dl;
    std::vector<std::string> log;

    explicit M1Fixture(bool demo = true, bool logging = true)
    {
        for (int t = 0; t < 16; ++t)
            ui::m1::loadProgress()[t] = -1;
        ui::ShellConfig cfg{core::builtinParams(), snapshot::measure()};
        if (logging) // log lines are strings (the app's trace choice): off where allocation is measured
            cfg.log = [this](const std::string& s) { log.push_back(s); };
        shell = std::make_unique<ui::Shell>(std::move(cfg));
        if (demo)
            snapshot::homeTracks(shell->state());
        settle();
    }
    ui::DrawList& settle(int frames = 2)
    {
        for (int i = 0; i < frames; ++i)
        {
            now += 1000;
            dl.clear();
            shell->frame(dl, now);
        }
        return dl;
    }
    ui::DrawList& frame(double stepMs = 1000.0 / 60.0)
    {
        now += stepMs;
        dl.clear();
        shell->frame(dl, now);
        return dl;
    }
    void send(io::ControlEvent e)
    {
        e.timeMs = now;
        shell->dispatch(e);
    }
    void press(io::Control c) { send({io::ControlEvent::Kind::Press, c, 100, io::kSourceVirtual, 0}); }
    void release(io::Control c) { send({io::ControlEvent::Kind::Release, c, 0, io::kSourceVirtual, 0}); }
    void turn(io::Control c, int d)
    {
        send({io::ControlEvent::Kind::Turn, c, static_cast<std::int16_t>(d), io::kSourceVirtual, 0});
    }
    // A tap at canvas (x, y): touch down + up (the touch events carry 800 × 480 points).
    void tap(float x, float y)
    {
        io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown, io::kSourceVirtual, 0};
        e.x = static_cast<std::int16_t>(x / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(y / ui::touch::kToCanvas + 0.5f);
        send(e);
        e.value = ui::touch::kUp;
        send(e);
    }
    void set(std::string_view id, int track, float v) { ui::m1::setTrackParam(*shell, id, track, v); }
    snapshot::Image image(int w = 1280, int h = 768) { return snapshot::render(settle(), w, h); }
};
} // namespace m1test

namespace m1test
{
// A pinned spec mockup (resources/design/mockups/, hashes in resources/design/SPEC_VERSION).
inline const snapshot::Image& m1mockup(std::string_view name)
{
    return snapshot::mockup(name);
}
} // namespace m1test
