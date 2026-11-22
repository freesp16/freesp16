// RENDER AUDIO (07-ui-design/screens/project.md "Layout — RENDER AUDIO", UID-858…860; 03-ui/screens.md
// RENDER; REN-001…005, ARR-009). Screen id `project.renderAudio` (the PROJECT list's row 7 opens it; the id the
// PROJECT screen already used), template grid, breadcrumb HOME › PROJECT › RENDER AUDIO, slot A = BACK. Moved here from
// ui/screens/project (where it was laid out behind flag `render`) and wired to the offline render through
// RenderHost (the RenderService in the product).
//   - RENDERED TARGET: MIX (OUT1-2 MASTER on by default; OUT3-4 … OUT7-8 stay disabled: the render service
//     renders the master mix and single tracks, a per-output stem needs a router tap — I-item) and INDIVIDUAL TRACKS
//     1–16 in pad order (empty tracks disabled). Each target that is on renders one file: MIX = the pattern (or the
//     song) with the send / master FX; a track = that track through its insert FX on a direct output (REN-001).
//   - RENDER MODE PATTERN | ARRANGEMENT (disabled while the arrangement is empty), REMAINDER CUT | WRAP | LEAVE
//     (render.remainder, REN-002), DESTINATION LIBRARY | EXTERNAL | TO TRACK (REN-003/004; TO TRACK needs one empty
//     track per file: the summary says so in `warn` and RENDER is disabled).
//   - Knobs: REPEATS (render.repeats walked 1×, 2×, 4×, 8×; disabled in ARRANGEMENT), BIT DEPTH (render.bitDepth,
//     `16-bit` / `24-bit` / `32-bit float`), NORMALIZE (render.normalize, REN-010).
//   - RENDER → the blocking Progress with CANCEL (live output muted meanwhile, REN-005); the end is a Toast with the
//     file count and the last file's analysis (peak, bars / BPM: REN-020/021); a failure or a cancel says so.
//   - Rotary: the tracks in pad order, then MIX, then the right column, then RENDER; push toggles / selects / renders.
#include "core/commands/arrangement/ArrangementCommands.h"
#include "ui/screens/render/RenderHost.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/Progress.h"
#include "ui/widgets/Segmented.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ui
{
namespace m5
{
namespace
{
RenderHost*& installed() noexcept
{
    static RenderHost* host = nullptr;
    return host;
}
} // namespace
RenderHost* RenderHost::current() noexcept
{
    return installed();
}
void RenderHost::install(RenderHost* h) noexcept
{
    installed() = h;
}
} // namespace m5

namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace rnd = ::services::render;
using Kind = io::ControlEvent::Kind;

constexpr std::string_view kModes[] = {"PATTERN", "ARRANGEMENT"};
constexpr std::string_view kRemainders[] = {"CUT", "WRAP", "LEAVE"};
constexpr std::string_view kDestinations[] = {"LIBRARY", "EXTERNAL", "TO TRACK"};
constexpr std::string_view kBitDepths[] = {"16-bit", "24-bit", "32-bit float"};
constexpr std::string_view kRepeatsText[] = {"1\xC3\x97", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97"};
constexpr int kRepeats[] = {1, 2, 4, 8};
constexpr RectF kRender {1072, 520, 192, 88};
constexpr RectF mixToggle(int i) noexcept
{
    return {32 + 188 * static_cast<float>(i), 212, 180, 60};
}
constexpr RectF trackToggle(int t) noexcept // pad order: 13–16 on top
{
    return {32 + 188 * static_cast<float>((t - 1) % 4), 316 + 68 * static_cast<float>(3 - (t - 1) / 4), 180, 60};
}
constexpr RectF kSegments[3] = {{808, 200, 456, 56}, {808, 304, 456, 56}, {808, 408, 456, 56}};
constexpr const char* kTrackLabels[] = {"TRACK 1",  "TRACK 2",  "TRACK 3",  "TRACK 4",  "TRACK 5",  "TRACK 6",
                                        "TRACK 7",  "TRACK 8",  "TRACK 9",  "TRACK 10", "TRACK 11", "TRACK 12",
                                        "TRACK 13", "TRACK 14", "TRACK 15", "TRACK 16"};
// Focus order (project.md "Interactions"): the tracks in pad order, then MIX, then the right column, then RENDER.
constexpr int kFocusCount = 16 + 4 + 3 + 1, kFocusMix = 16, kFocusMode = 20, kFocusRemainder = 21,
              kFocusDestination = 22, kFocusRender = 23;
constexpr int trackAtFocus(int f) noexcept // focus 0–15 → track 1–16 in pad order
{
    return 13 - 4 * (f / 4) + f % 4;
}
constexpr std::uint8_t kMixEnabled = 1; // OUT1-2 MASTER only (OUT3-4 … OUT7-8: see the header)

// The screen's choices for the session (OUT1-2 MASTER on by default; kept while the app runs).
struct Memory
{
    std::uint8_t mix = 1;
    std::uint16_t tracks = 0;
    int mode = 0, destination = 0, focus = 0;
};
Memory& memory()
{
    static Memory m;
    return m;
}

class RenderAudio final : public Widget, public StripOwner
{
public:
    explicit RenderAudio(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state)
    {
        if (auto* h = m5::RenderHost::current())
            seenFinished_ = h->status().finished;
    }
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }
    int focus() const noexcept { return memory().focus; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        auto& mem = memory();
        poll();
        if (! arrangementAvailable())
            mem.mode = 0;
        const int focus = mem.focus;
        dl.fill({16, 152, 776, 456}, p.surface1, tk::radiusM);
        dl.stroke({16.5f, 152.5f, 775, 455}, p.border, 1, tk::radiusM);
        caption(dl, ctx, 32, 172, 744, "RENDERED TARGET", p.surface1);
        caption(dl, ctx, 32, 200, 744, "MIX", p.surface1);
        static constexpr std::string_view mix[4][2] = {
            {"OUT1-2", "MASTER"}, {"OUT3-4", {}}, {"OUT5-6", {}}, {"OUT7-8", {}}};
        for (int i = 0; i < 4; ++i)
            ledToggle(dl, ctx, mixToggle(i), mix[i][0], mix[i][1], (mem.mix >> i & 1u) != 0,
                      (kMixEnabled >> i & 1u) == 0, focus == kFocusMix + i);
        caption(dl, ctx, 32, 300, 744, "INDIVIDUAL TRACKS", p.surface1);
        for (int f = 0; f < 16; ++f)
        {
            const int t = trackAtFocus(f);
            ledToggle(dl, ctx, trackToggle(t), kTrackLabels[t - 1], {}, (mem.tracks >> (t - 1) & 1u) != 0, empty(t),
                      focus == f);
        }
        static constexpr std::string_view captions[3] = {"RENDER MODE", "REMAINDER", "DESTINATION"};
        const std::span<const std::string_view> items[3] = {kModes, kRemainders, kDestinations};
        const int selected[3] = {mem.mode, remainder(), mem.destination};
        for (int k = 0; k < 3; ++k)
        {
            caption(dl, ctx, 808, kSegments[k].y - 16, 456, captions[k], p.bg);
            SegmentedModel m {items[k], selected[k], k == 0 && ! arrangementAvailable() ? 2u : 0u, {}, 0,
                              focus == kFocusMode + k};
            Segmented::draw(dl, ctx, kSegments[k], m);
        }
        const int files = targets();
        const int free = freeTracks();
        bool blocked = files == 0;
        std::uint32_t ink = p.text2;
        if (files == 0)
            summary_.set("Choose at least one target");
        else if (mem.destination == 2 && free < files)
        {
            summary_.format("Needs %d empty track%s, %d free", files, files == 1 ? "" : "s", free);
            ink = p.warn;
            blocked = true;
        }
        else
        {
            const auto where = mem.destination == 2 ? std::string_view("empty tracks") : destination();
            summary_.format("%d file%s \xE2\x86\x92 %.*s", files, files == 1 ? "" : "s", static_cast<int>(where.size()),
                            where.data());
        }
        dl.text(808, 492, Align::Left, TextStyleId::Body, ink, summary_.view());
        Button::draw(dl, ctx, kRender, {"RENDER", ButtonStyle::Primary, blocked || busy(), false, focus == kFocusRender});
    }

    bool dirty(double) const override { return busy() || progress_ != nullptr; }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr)
            return false;
        auto& mem = memory();
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            mem.focus = std::clamp(mem.focus + e.value, 0, kFocusCount - 1);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            activate(mem.focus, -1);
            return true;
        }
        if (! touch::isTouch(e) || e.value != touch::kDown)
            return false;
        const float x = touch::x(e), y = touch::y(e);
        for (int f = 0; f < 16; ++f)
            if (contains(trackToggle(trackAtFocus(f)), x, y))
            {
                activate(f, -1);
                return true;
            }
        for (int i = 0; i < 4; ++i)
            if (contains(mixToggle(i), x, y))
            {
                activate(kFocusMix + i, -1);
                return true;
            }
        for (int k = 0; k < 3; ++k)
            if (contains(kSegments[k], x, y))
            {
                const int n = k == 0 ? 2 : 3;
                activate(kFocusMode + k,
                         std::clamp(static_cast<int>((x - kSegments[k].x) / (kSegments[k].w / static_cast<float>(n))),
                                    0, n - 1));
                return true;
            }
        if (contains(kRender, x, y))
        {
            activate(kFocusRender, -1);
            return true;
        }
        return false;
    }

    // --- StripOwner: REPEATS as 1× / 2× / 4× / 8×, BIT DEPTH spelled out (project.md "Knob strip") -----------------
    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (c.spec == nullptr)
            return;
        if (column0 == 0)
        {
            c.valueText = kRepeatsText[repeatIndex()];
            c.stepped = true;
            c.disabled = memory().mode == 1; // ARRANGEMENT renders the song once
        }
        else if (column0 == 1)
            c.valueText = kBitDepths[std::clamp(static_cast<int>(std::lround(c.value)), 0, 2)];
    }
    bool stripTurn(int column0, const core::ParamSpec& spec, int detents, bool) override
    {
        if (column0 != 0 || shell_ == nullptr)
            return false;
        const int i = std::clamp(repeatIndex() + detents, 0, 3);
        shell_->values().set(spec, 0, static_cast<float>(kRepeats[i]));
        return true;
    }

private:
    float value(std::string_view id, float fallback = 0) const
    {
        if (shell_ == nullptr)
            return fallback;
        const auto* spec = shell_->params().find(id);
        return spec != nullptr ? shell_->values().get(*spec, 0) : fallback;
    }
    int repeatIndex() const
    {
        const int r = static_cast<int>(std::lround(value("render.repeats", 1)));
        return r >= 8 ? 3 : r >= 4 ? 2 : r >= 2 ? 1 : 0;
    }
    int remainder() const { return std::clamp(static_cast<int>(std::lround(value("render.remainder"))), 0, 2); }
    static bool arrangementAvailable()
    {
        return core::commands::arrangement::rowCount(m2::Session::instance().project()) > 0;
    }
    static bool busy()
    {
        const auto* h = m5::RenderHost::current();
        return h != nullptr && h->status().busy;
    }
    std::string_view destination() const
    {
        const auto* h = m5::RenderHost::current();
        if (h == nullptr)
            return memory().destination == 1 ? std::string_view("the chosen folder")
                                             : std::string_view("Library / RenderedAudio");
        return h->destinationName(memory().destination == 1);
    }
    bool empty(int t) const
    {
        return state_ != nullptr && ! state_->tracks[static_cast<std::size_t>(t - 1)].hasContent;
    }
    int targets() const
    {
        auto& mem = memory();
        int n = 0;
        for (int i = 0; i < 4; ++i)
            n += (mem.mix >> i & 1u) != 0 && (kMixEnabled >> i & 1u) != 0 ? 1 : 0;
        for (int t = 1; t <= 16; ++t)
            n += (mem.tracks >> (t - 1) & 1u) != 0 && ! empty(t) ? 1 : 0;
        return n;
    }
    static int freeTracks()
    {
        const auto& p = m2::Session::instance().project();
        const auto& scene = p.scenes[static_cast<std::size_t>(p.settings.currentScene & 15)];
        int n = 0;
        for (const auto& t : scene.tracks)
            n += t.playback.sample.empty() && t.mode == core::model::TrackMode::Sample ? 1 : 0;
        return n;
    }
    void activate(int f, int segment)
    {
        auto& mem = memory();
        mem.focus = f;
        if (f < 16)
        {
            const int t = trackAtFocus(f);
            if (! empty(t))
                mem.tracks = static_cast<std::uint16_t>(mem.tracks ^ (1u << (t - 1)));
        }
        else if (f < kFocusMode)
        {
            if ((kMixEnabled >> (f - kFocusMix) & 1u) != 0)
                mem.mix = static_cast<std::uint8_t>(mem.mix ^ (1u << (f - kFocusMix)));
        }
        else if (f == kFocusMode)
        {
            const int want = segment >= 0 ? segment : (mem.mode + 1) % 2;
            mem.mode = want == 1 && ! arrangementAvailable() ? 0 : want;
        }
        else if (f == kFocusRemainder)
        {
            if (const auto* spec = shell_->params().find("render.remainder"))
                shell_->values().set(*spec, 0, static_cast<float>(segment >= 0 ? segment : (remainder() + 1) % 3));
        }
        else if (f == kFocusDestination)
            mem.destination = segment >= 0 ? segment : (mem.destination + 1) % 3;
        else
            render();
    }

    // The batch: MIX first, then the tracks in number order; one job each (REN-001).
    void render()
    {
        auto& mem = memory();
        const int files = targets();
        if (files == 0 || busy())
            return;
        auto* host = m5::RenderHost::current();
        if (host == nullptr)
        {
            toast("Rendering is not available");
            return;
        }
        const auto& p = m2::Session::instance().project();
        const int scene = p.settings.currentScene & 15, pattern = p.settings.currentPattern & 15;
        const bool toTrack = mem.destination == 2;
        if (toTrack && freeTracks() < files)
            return;
        std::vector<int> free;
        for (int t = 0; t < 16; ++t)
        {
            const auto& tr = p.scenes[static_cast<std::size_t>(scene)].tracks[static_cast<std::size_t>(t)];
            if (tr.playback.sample.empty() && tr.mode == core::model::TrackMode::Sample)
                free.push_back(t);
        }
        auto base = rnd::jobFromPreferences([this](std::string_view id) { return value(id); }, scene, pattern, 0);
        base.repeats = kRepeats[repeatIndex()];
        base.bars = 0;
        base.toTrack = toTrack;
        std::vector<rnd::RenderJob> jobs;
        std::size_t next = 0;
        auto add = [&](rnd::RenderJob j)
        {
            if (toTrack)
                j.targetTrack = free[next++];
            jobs.push_back(j);
        };
        if ((mem.mix & kMixEnabled & 1u) != 0)
        {
            auto j = base;
            j.source = mem.mode == 1 ? rnd::Source::Arrangement : rnd::Source::Pattern;
            j.includeBusFx = true;
            add(j);
        }
        for (int t = 1; t <= 16; ++t)
            if ((mem.tracks >> (t - 1) & 1u) != 0 && ! empty(t))
            {
                auto j = base;
                j.source = rnd::Source::Track;
                j.track = t - 1;
                j.includeBusFx = false; // through its insert FX, as a direct output (REN-001, FX-012)
                add(j);
            }
        if (! host->start(std::move(jobs), mem.destination == 1))
        {
            toast(host->status().error.empty() ? std::string("Render refused") : host->status().error);
            return;
        }
        ProgressRequest req;
        req.title = "RENDERING";
        req.detail = files == 1 ? std::string("1 file") : std::to_string(files) + " files";
        req.cancellable = true;
        req.onCancel = [this]
        {
            progress_ = nullptr; // the popup closes itself
            if (auto* h = m5::RenderHost::current())
                h->cancel();
        };
        progress_ = shell_->showProgress(std::move(req));
    }

    // Once per frame: the batch's progress into the popup, its end into a Toast.
    void poll() const
    {
        const auto* host = m5::RenderHost::current();
        if (host == nullptr)
            return;
        const auto& s = host->status();
        if (progress_ != nullptr && s.busy)
        {
            detail_.format("File %d of %d", s.index + 1, s.count);
            progress_->set(s.fraction, detail_.view());
        }
        if (s.finished == seenFinished_)
            return;
        seenFinished_ = s.finished;
        if (progress_ != nullptr)
        {
            progress_->done();
            progress_ = nullptr;
        }
        if (shell_ == nullptr)
            return;
        std::string text;
        if (s.cancelled)
            text = "Render cancelled";
        else if (s.failed)
            text = "Render failed: " + s.error;
        else
        {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%d file%s rendered \xC2\xB7 peak %.1f dBFS \xC2\xB7 %.2f bars %.1f BPM",
                          s.written, s.written == 1 ? "" : "s", s.peakDb, s.bars, s.bpm);
            text = buf;
        }
        ToastRequest t;
        t.text = std::move(text);
        shell_->showToast(std::move(t));
    }
    void toast(std::string text) const
    {
        ToastRequest t;
        t.text = std::move(text);
        shell_->showToast(std::move(t));
    }
    void caption(DrawList& dl, const UiContext& ctx, float x, float y, float w, std::string_view text,
                 std::uint32_t bg) const
    {
        const float cw = ctx.measure ? ctx.measure(TextStyleId::Caption, text) : 8.0f * static_cast<float>(text.size());
        draw::captionLine(dl, ctx.palette, x, y, w, text, cw, bg);
    }
    // kit_global led_toggle: surface-2 (surface-1 disabled), LED (w − 24) × 4 at y + 7, one or two label lines.
    static void ledToggle(DrawList& dl, const UiContext& ctx, RectF r, std::string_view l1, std::string_view l2,
                          bool on, bool disabled, bool focused)
    {
        const auto& p = ctx.palette;
        dl.fill(r, disabled ? p.surface1 : p.surface2, tk::radiusS);
        dl.fill({r.x + 12, r.y + 7, r.w - 24, 4}, on && ! disabled ? p.accent : (disabled ? p.border : p.borderStrong),
                2);
        const auto ink = disabled ? p.text3 : p.text1;
        const float cy = r.y + r.h / 2 + 4;
        if (l2.empty())
            dl.text(r.x + r.w / 2, cy, Align::Centre, TextStyleId::Label, ink, l1);
        else
        {
            dl.text(r.x + r.w / 2, cy - 8, Align::Centre, TextStyleId::Label, ink, l1);
            dl.text(r.x + r.w / 2, cy + 8, Align::Centre, TextStyleId::Label, ink, l2);
        }
        if (focused)
            draw::focusRing(dl, p, r, tk::radiusS);
    }

    ShellApi* shell_;
    UiState* state_;
    mutable Progress* progress_ = nullptr;
    mutable std::uint32_t seenFinished_ = 0;
    mutable FixedText<64> summary_;
    mutable FixedText<32> detail_;
};

std::unique_ptr<Widget> makeRenderAudio(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<RenderAudio>(a);
}
} // namespace

int renderFocus(const Widget* body) noexcept
{
    const auto* r = dynamic_cast<const RenderAudio*>(body);
    return r != nullptr ? r->focus() : -1;
}
void resetRenderScreen() noexcept
{
    memory() = Memory {};
}
} // namespace ui

APP_REGISTER_WIDGET("RenderAudio", &ui::makeRenderAudio)
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "project.renderAudio",
                                     .title = "RENDER AUDIO",
                                     .parent = "project",
                                     .body = ui::BodyTemplate::Grid,
                                     .contextBar = ui::ContextBarKind::Menu,
                                     .rail = {},
                                     .bodyComponent = "RenderAudio",
                                     .strip = {"RENDER AUDIO", {"render.repeats", "render.bitDepth", "render.normalize"},
                                               0}}))
