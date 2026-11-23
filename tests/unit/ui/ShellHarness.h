// A Shell on a private screen registry with the fallback HOME, driven by synthetic control events.
#pragma once

#include "core/params/ParamRegistry.h"
#include "ui/shell/Shell.h"

#include <memory>
#include <string>
#include <vector>

namespace uitest
{
inline ui::TextMeasure fixedMeasure()
{
    return [](ui::TextStyleId, std::string_view t) { return 8.0f * static_cast<float>(t.size()); };
}

struct Harness
{
    ui::ScreenRegistry screens;
    std::unique_ptr<ui::Shell> shell;
    std::vector<std::string> log;
    ui::DrawList dl;
    double now = 1000;

    explicit Harness(std::initializer_list<ui::ScreenSpec> extra = {}, ui::FeatureFlags flags = {},
                     bool valueEntry = true)
    {
        for (const auto& s : extra)
            screens.add(s);
        ui::ShellConfig cfg{core::builtinParams(), fixedMeasure()};
        cfg.screens = &screens;
        cfg.flags = flags;
        cfg.valueEntry = valueEntry;
        cfg.log = [this](const std::string& l) { log.push_back(l); };
        shell = std::make_unique<ui::Shell>(std::move(cfg));
        dl.reserve(4096, 32768);
        frame();
    }
    ui::DrawList& frame(double advanceMs = 500)
    {
        now += advanceMs;
        dl.clear();
        shell->frame(dl, now);
        return dl;
    }
    void send(io::ControlEvent::Kind k, io::Control c, int v = 0)
    {
        shell->dispatch({k, c, static_cast<std::int16_t>(v), io::kSourceVirtual, now});
    }
    void press(io::Control c, int v = 100) { send(io::ControlEvent::Kind::Press, c, v); }
    void release(io::Control c) { send(io::ControlEvent::Kind::Release, c); }
    void tap(io::Control c)
    {
        press(c);
        release(c);
    }
    void turn(io::Control c, int d) { send(io::ControlEvent::Kind::Turn, c, d); }
    // A touch at a canvas point (the event carries the 800 × 480 logical point, ×1.6 = canvas).
    void touch(float cx, float cy, std::int16_t phase = ui::touch::kDown)
    {
        io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, phase, io::kSourceVirtual, now};
        e.x = static_cast<std::int16_t>(cx / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(cy / ui::touch::kToCanvas + 0.5f);
        shell->dispatch(e);
    }
    float value(std::string_view id, int track = 0)
    {
        const auto* p = core::builtinParams().find(id);
        return shell->values().get(*p, shell->state().trackScope(track));
    }
    // True when the frame draws a full-canvas fill in the overlay (scrim) colour.
    bool hasScrim()
    {
        const auto overlay = ui::theme::palette().overlay;
        for (const auto& c : dl.commands())
            if (c.op == ui::DrawCmd::Op::Fill && c.argb == overlay && c.rect.w == 1280 && c.rect.h == 768)
                return true;
        return false;
    }
    bool drewText(std::string_view text)
    {
        for (const auto& c : dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == text)
                return true;
        return false;
    }
};
} // namespace uitest
