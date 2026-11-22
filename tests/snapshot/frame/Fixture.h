// Fixture data equal to the mockups' demo data (07-ui-design/_tools/screens/home.py TRACKS, kit_track.py SAMPLES), so
// a snapshot compares like with like.
#pragma once

#include "core/params/ParamRegistry.h"
#include "tests/snapshot/SnapshotTest.h"
#include "ui/shell/Shell.h"

#include <memory>

namespace snapshot
{
inline void homeTracks(ui::UiState& s)
{
    struct T
    {
        int n;
        std::string_view type, name, stretch, loop;
    };
    static constexpr T tracks[] = {{1, "SMPL", "Kick_01", "OFF", "\xE2\x86\x92"},
                                   {2, "SMPL", "Snare_01", "OFF", "\xE2\x86\x92"},
                                   {3, "SMPL", "Hat_Closed_01", "OFF", "\xE2\x86\x92"},
                                   {5, "SMPL", "Beat_120bpm_2bar", "M.TMP", "\xE2\x9F\xB2"},
                                   {6, "SMPL", "Bass_120bpm_2bar", "M.TMP", "\xE2\x9F\xB2"},
                                   {10, "MIDI", "MIDI CH.10", "", ""},
                                   {11, "THRU", "SOURCE \xC2\xB7 LINE IN", "", ""}};
    for (const auto& t : tracks)
    {
        auto& x = s.tracks[static_cast<std::size_t>(t.n - 1)];
        x.hasContent = true;
        x.typeBadge = t.type;
        x.name = t.name;
        x.stretchBadge = t.stretch;
        x.loopGlyph = t.loop;
        x.levelDb = t.n == 1 || t.n == 5 ? -25.0f : -120.0f; // the mockup lights 3 mini-level segments on 1 and 5
    }
}

struct ShellFixture
{
    ui::ScreenRegistry screens;
    std::unique_ptr<ui::Shell> shell;
    double now = 10000;

    explicit ShellFixture(std::initializer_list<ui::ScreenSpec> extra = {})
    {
        for (const auto& s : extra)
            screens.add(s);
        ui::ShellConfig cfg{core::builtinParams(), measure()};
        cfg.screens = &screens;
        shell = std::make_unique<ui::Shell>(std::move(cfg));
        homeTracks(shell->state());
        settle();
    }
    // Builds frames until every transition has ended; returns the last frame.
    ui::DrawList& settle()
    {
        for (int i = 0; i < 2; ++i)
        {
            now += 1000;
            dl.clear();
            shell->frame(dl, now);
        }
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
    Image image(int w = 1280, int h = 768) { return render(settle(), w, h); }
    ui::DrawList dl;
};
} // namespace snapshot
