// The sequencing screens through the real shell with the built-in registries (screens, popups, widgets, parameters),
// both interim parameter-stub flags on (ui.m1.paramStub, ui.m2.paramStub), a fresh m2::Session (default project on the
// session's own CommandBus) and the mockups' demo data.
#pragma once

#include "tests/snapshot/m1/M1Fixture.h"
#include "tests/snapshot/m2/FakeProjectSource.h"
#include "ui/screens/project/ProjectAccess.h"
#include "ui/screens/project/ProjectSource.h"
#include "ui/screens/scenemanager/SceneManagerAccess.h"
#include "ui/screens/sequence/M2Commands.h"
#include "ui/screens/sequence/M2Session.h"

#include <string>
#include <vector>

namespace m2test
{
struct M2Fixture
{
    std::unique_ptr<ui::Shell> shell;
    double now = 10000;
    ui::DrawList dl;
    std::vector<std::string> log;

    explicit M2Fixture(bool demo = true, bool logging = true)
    {
        ui::m2::Session::instance().reset();
        FakeProjectSource::instance().reset();
        ui::m2::ProjectSource::install(&FakeProjectSource::instance());
        ui::resetProjectScreens();
        ui::resetSceneManager();
        for (int t = 0; t < 16; ++t)
            ui::m1::loadProgress()[t] = -1;
        ui::ShellConfig cfg{core::builtinParams(), snapshot::measure()};
        cfg.flags.enable(ui::m2::kParamStubFlag);
        cfg.flags.enable(ui::m2::kRenderFlag);
        if (logging) // log lines are strings (the app's trace choice): off where allocation is measured
            cfg.log = [this](const std::string& s) { log.push_back(s); };
        shell = std::make_unique<ui::Shell>(std::move(cfg));
        if (demo)
            snapshot::homeTracks(shell->state());
        settle();
    }
    ~M2Fixture()
    {
        shell.reset();
        ui::m2::Session::instance().reset();
        FakeProjectSource::instance().reset();
        ui::m2::ProjectSource::install(nullptr);
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
    void click(io::Control c)
    {
        press(c);
        release(c);
    }
    void turn(io::Control c, int d)
    {
        send({io::ControlEvent::Kind::Turn, c, static_cast<std::int16_t>(d), io::kSourceVirtual, 0});
    }
    void touch(float x, float y, std::int16_t phase)
    {
        io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, phase, io::kSourceVirtual, 0};
        e.x = static_cast<std::int16_t>(x / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(y / ui::touch::kToCanvas + 0.5f);
        send(e);
    }
    void tap(float x, float y)
    {
        touch(x, y, ui::touch::kDown);
        touch(x, y, ui::touch::kUp);
    }
    void set(std::string_view id, int track, float v) { ui::m1::setTrackParam(*shell, id, track, v); }
    float get(std::string_view id, int track = 0)
    {
        return ui::m1::trackParam(*shell, id, track > 0 ? track : shell->state().selectedTrack);
    }
    snapshot::Image image(int w = 1280, int h = 768) { return snapshot::render(settle(), w, h); }
    ui::m2::Session& session() { return ui::m2::Session::instance(); }
    // Writes a trigger (0-based step) on the current scene/pattern of track t (1-based).
    void trigger(int t, int step, core::model::TriggerType type = core::model::TriggerType::Full)
    {
        auto& s = session();
        s.execute(ui::m2::cmd::toggleStep(s.project(), shell->state().scene - 1, shell->state().pattern - 1, t - 1,
                                          step, type, 127));
    }
    void lock(int t, int step, core::model::LockSlot slot, float v)
    {
        auto& s = session();
        const int steps[] = {step};
        const float values[] = {v};
        s.execute(ui::m2::cmd::setLocks(s.project(), shell->state().scene - 1, shell->state().pattern - 1, t - 1, steps,
                                        slot, values, -9999, "LOCK"));
    }
    bool open(std::string_view id)
    {
        const bool ok = shell->open(id);
        settle();
        return ok;
    }
};

// A mockup from resources/design/mockups/ (pinned) or tests/snapshot/m2/mockups/.
inline const snapshot::Image& m2mockup(std::string_view name)
{
    static const char* pinned[] = {"save-as", "message-confirm"};
    for (const auto* p : pinned)
        if (name == p)
            return snapshot::mockup(name);
    static std::vector<std::string> keys;
    keys.reserve(32);
    keys.push_back("../../../tests/snapshot/m2/mockups/" + std::string(name));
    return snapshot::mockup(keys.back());
}

inline bool hasText(const ui::DrawList& dl, std::string_view text)
{
    for (const auto& c : dl.commands())
        if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == text)
            return true;
    return false;
}
} // namespace m2test
