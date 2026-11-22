// Component behaviour that the snapshot tests do not cover: breadcrumb collapse (UID-300), TextField UTF-8 editing,
// SoftwareKeyboard geometry and layers, GridPopup paging and one-tap choice (UI-262), Segmented, theme motion helpers,
// the exact waveform columns (WaveOverview::ch).
#include "tests/unit/ui/ShellHarness.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/GridPopup.h"
#include "ui/widgets/Segmented.h"
#include "ui/widgets/SoftwareKeyboard.h"
#include "ui/widgets/StatusRow.h"
#include "ui/widgets/TextField.h"
#include "ui/widgets/WaveformEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

TEST_CASE("widgets: the breadcrumb collapses middle segments to … beyond 560 lpx (UID-300)", "[ui][widgets]")
{
    const auto m = uitest::fixedMeasure(); // 8 lpx per byte
    std::array<int, ui::StatusRow::kMaxSegments> idx{};
    static constexpr std::string_view shortPath[] = {"HOME", "TRACK MENU", "PLAYBACK"};
    CHECK(ui::StatusRow::layout(shortPath, m, idx) == 3);
    static constexpr std::string_view longPath[] = {
        "HOME", "TRACK MENU", "PLAYBACK", "SET SLICE GRID", "A VERY LONG SCREEN", "ANOTHER LONG SCREEN TITLE"};
    const int n = ui::StatusRow::layout(longPath, m, idx);
    REQUIRE(n >= 3);
    CHECK(idx[0] == 0);  // HOME stays
    CHECK(idx[1] == -1); // …
    CHECK(idx[static_cast<std::size_t>(n - 1)] == 5);
}

TEST_CASE("widgets: TextField edits UTF-8 without splitting code points", "[ui][widgets]")
{
    ui::TextField f;
    f.setText("Beat", true);
    CHECK(f.allSelected());
    f.insert("K\xC3\xA4"); // replaces the selection
    CHECK(f.text() == "K\xC3\xA4");
    CHECK(f.characters() == 2);
    f.moveCaret(-1);
    CHECK(f.caret() == 1);
    f.backspace();
    CHECK(f.text() == "\xC3\xA4");
    f.moveCaret(5);
    f.backspace();
    CHECK(f.text().empty());
    f.setText("one two_three", false);
    f.moveCaret(-100);
    f.moveWord(1);
    CHECK(f.caret() == 3);
    f.moveWord(1);
    CHECK(f.caret() == 7);
}

TEST_CASE("widgets: SoftwareKeyboard rows follow project.md (keys 52 tall, pitch 60, first row y 528)", "[ui][widgets]")
{
    ui::SoftwareKeyboard k;
    using KK = ui::SoftwareKeyboard::KeyKind;
    CHECK(k.key(0).rect.y == 528);
    CHECK(k.key(0).rect.x == 28);
    CHECK(k.key(0).text == "q");
    CHECK(k.key(10).kind == KK::Backspace);
    CHECK(k.key(20).kind == KK::Done);
    CHECK(k.key(20).rect.w == 216);
    CHECK(k.key(21).kind == KK::Shift);
    CHECK(k.key(33).kind == KK::Space);
    CHECK(k.key(33).rect.w == 704);
    CHECK(k.key(35).rect.y == 528 + 3 * 60);
    for (int i = 0; i < ui::SoftwareKeyboard::kKeyCount; ++i)
    {
        const auto r = k.key(i).rect;
        CHECK(r.h == 52);
        CHECK(r.x >= 0);
        CHECK(r.x + r.w <= 1280);
        CHECK(k.keyAt(r.x + r.w / 2, r.y + r.h / 2) == i);
    }
    k.press(21); // ⇧
    CHECK(k.key(0).text == "Q");
    k.press(32); // 123
    CHECK(k.key(0).text == "1");
    CHECK(k.key(11).text == "(");
}

TEST_CASE("widgets: GridPopup hides unavailable cells, pages beyond 15 and chooses on one tap (UID-216, UI-262)",
          "[ui][widgets]")
{
    ui::GridRequest r;
    r.title = "SELECT FX";
    int chosen = -1;
    for (int i = 0; i < 18; ++i)
        r.items.push_back({"FX", "fx", i != 3});
    r.current = 16;
    r.onChoose = [&](int i) { chosen = i; };
    ui::GridPopup g(std::move(r));
    CHECK(g.pages() == 2);
    CHECK(g.focusedItem() == 16);
    CHECK(g.page() == 1);
    ui::FocusState focus;
    const auto& p = ui::theme::palette();
    ui::UiContext ctx{p, uitest::fixedMeasure(), 0, focus};
    const auto c = g.cell(0);
    io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown, 0, 0};
    e.x = static_cast<std::int16_t>((c.x + 10) / ui::touch::kToCanvas);
    e.y = static_cast<std::int16_t>((c.y + 10) / ui::touch::kToCanvas);
    g.handle(e, ctx);
    CHECK(chosen == 16); // page 2's first cell: item 16 (item 3 hidden, cells close up)
    CHECK(g.closed);
}

TEST_CASE("widgets: Segmented steps over disabled segments without wrapping", "[ui][widgets]")
{
    static constexpr std::string_view items[] = {"BAR1", "BAR2", "BAR3", "BAR4"};
    ui::Segmented s({120, 516, 1040, 88}, {items, 0, 0b0010, {}, 0, true});
    ui::FocusState focus;
    ui::UiContext ctx{ui::theme::palette(), uitest::fixedMeasure(), 0, focus};
    s.handle({io::ControlEvent::Kind::Turn, io::Control::Selector, 1, 0, 0}, ctx);
    CHECK(s.model().selected == 2);
    s.handle({io::ControlEvent::Kind::Turn, io::Control::Selector, 1, 0, 0}, ctx);
    s.handle({io::ControlEvent::Kind::Turn, io::Control::Selector, 1, 0, 0}, ctx);
    CHECK(s.model().selected == 3);
    CHECK(s.segmentAt(120 + 260 + 10, 530) == -1); // disabled
}

TEST_CASE("theme: easing, fades and blinks come from the motion tokens (UID-065, UID-066)", "[ui][theme]")
{
    CHECK(ui::theme::ease(0) == 0.0f);
    CHECK(ui::theme::ease(1) > 0.999f);
    CHECK(ui::theme::ease(0.5f) > 0.5f);           // the standard curve decelerates
    CHECK(ui::theme::progress(100, 0, 0) == 1.0f); // reduce motion: duration 0 = done
    const auto faded = ui::theme::faded(ui::theme::palette(), 0.5f);
    CHECK((faded.text1 >> 24) == 128);
    CHECK(ui::theme::blinkOn(100, ::theme::tokens::motionBlinkSlowMs));
    CHECK_FALSE(ui::theme::blinkOn(600, ::theme::tokens::motionBlinkSlowMs));
}

namespace
{
// A stereo signal with its pool-style pyramid (64-frame bins, 4× steps) and the samples behind it.
struct ExactWave
{
    std::int64_t frames;
    std::vector<float> l, r;
    std::vector<std::vector<float>> mins, maxs;
    std::vector<ui::WaveOverview::Level> levels;
    const float* ch[2]{};
    ui::WaveOverview o;

    explicit ExactWave(std::int64_t f) : frames(f), l(static_cast<std::size_t>(f)), r(static_cast<std::size_t>(f))
    {
        std::uint32_t seed = 12345;
        for (std::int64_t i = 0; i < f; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const auto t = static_cast<double>(i);
            l[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(t * 0.013) * std::exp(-t / 60000.0));
            r[static_cast<std::size_t>(i)] = static_cast<float>(((seed >> 8) & 0xffff) / 65535.0 * 0.2 - 0.1);
        }
        l[77777] = 0.97f;  // one-sample spikes: only the column holding them may reach them
        r[150001] = -0.93f;
        std::vector<float> lo, hi;
        for (std::int64_t b = 0; b * 64 < f; ++b)
        {
            float mn = 1e9f, mx = -1e9f;
            for (std::int64_t i = b * 64; i < std::min(f, b * 64 + 64); ++i)
                for (const auto* c : {&l, &r})
                    mn = std::min(mn, (*c)[static_cast<std::size_t>(i)]), mx = std::max(mx, (*c)[static_cast<std::size_t>(i)]);
            lo.push_back(mn), hi.push_back(mx);
        }
        mins.push_back(lo), maxs.push_back(hi);
        while (mins.back().size() > 1)
        {
            std::vector<float> a, b;
            const auto& pl = mins.back();
            const auto& ph = maxs.back();
            for (std::size_t i = 0; i < pl.size(); i += 4)
            {
                const auto e = std::min(pl.size(), i + 4);
                a.push_back(*std::min_element(pl.begin() + static_cast<std::ptrdiff_t>(i), pl.begin() + static_cast<std::ptrdiff_t>(e)));
                b.push_back(*std::max_element(ph.begin() + static_cast<std::ptrdiff_t>(i), ph.begin() + static_cast<std::ptrdiff_t>(e)));
            }
            mins.push_back(std::move(a)), maxs.push_back(std::move(b));
        }
        double fpb = 64;
        for (std::size_t k = 0; k < mins.size(); ++k, fpb *= 4)
            levels.push_back({mins[k].data(), maxs[k].data(), static_cast<std::int64_t>(mins[k].size()), fpb});
        ch[0] = l.data(), ch[1] = r.data();
        o.levels = levels;
        o.frames = f;
        o.ch = ch;
        o.channels = 2;
    }
    // The reference: the straight line between samples over [a, b], read sample by sample.
    float at(const std::vector<float>& c, double t) const
    {
        t = std::clamp(t, 0.0, static_cast<double>(frames - 1));
        const auto i = static_cast<std::size_t>(t), j = std::min(i + 1, static_cast<std::size_t>(frames - 1));
        return c[i] + (c[j] - c[i]) * static_cast<float>(t - static_cast<double>(i));
    }
    void span(double a, double b, float& mn, float& mx) const
    {
        mn = 1e9f, mx = -1e9f;
        for (const auto* c : {&l, &r})
        {
            for (double t : {a, b})
                mn = std::min(mn, at(*c, t)), mx = std::max(mx, at(*c, t));
            for (auto i = static_cast<std::int64_t>(std::ceil(a)); i < std::min(frames, static_cast<std::int64_t>(std::ceil(b))); ++i)
                mn = std::min(mn, (*c)[static_cast<std::size_t>(i)]), mx = std::max(mx, (*c)[static_cast<std::size_t>(i)]);
        }
    }
};
} // namespace

TEST_CASE("widgets: waveform columns are the exact min / max of their own frames at every zoom", "[ui][widgets]")
{
    const ExactWave w(200003); // not a whole number of bins: the last bin of every level is partial
    constexpr int columns = 315;
    constexpr float cy = 300, h = 100, width = 1.8f, half = width / 2;
    struct Window
    {
        double f0, f1;
    };
    for (const Window win : {Window{0, 200003}, Window{70000, 90000}, Window{77000, 78000}, Window{77700, 77800},
                             Window{149900.5, 150100.25}, Window{12345.6, 12350.1}})
    {
        ui::DrawList dl;
        ui::WaveformEditor::drawColumns(dl, w.o, win.f0, win.f1, columns, 0, 3, width, cy, h, 0, w.frames, 0xff00ff00u);
        REQUIRE(dl.commands().size() == columns);
        const double per = (win.f1 - win.f0) / columns;
        for (int i = 0; i < columns; ++i)
        {
            float mn = 0, mx = 0;
            w.span(win.f0 + per * i, win.f0 + per * i + per, mn, mx);
            const auto& rc = dl.commands()[static_cast<std::size_t>(i)].rect;
            const float hi = (cy - (rc.y + half)) / h, lo = (cy - (rc.y + rc.h - half)) / h;
            INFO("window " << win.f0 << "…" << win.f1 << " column " << i);
            CHECK(std::abs(hi - mx) < 1e-4f);
            CHECK(std::abs(lo - mn) < 1e-4f);
        }
    }
}

TEST_CASE("widgets: a one-sample spike lights one waveform column, not the bins around it", "[ui][widgets]")
{
    const ExactWave w(200003);
    ui::DrawList dl;
    const double f0 = 76000, f1 = 76000 + 315 * 30.0; // 30 frames a column: under half a finest bin
    ui::WaveformEditor::drawColumns(dl, w.o, f0, f1, 315, 0, 3, 1.8f, 300, 100, 0, w.frames, 0xff00ff00u);
    int above = 0;
    for (const auto& c : dl.commands())
        above += (300 - (c.rect.y + 0.9f)) / 100 > 0.9f;
    CHECK(above == 1); // the coarse path drew it across the 64-frame bin's columns, and the ones it touched
}
