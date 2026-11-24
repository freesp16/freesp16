// LFO (07-ui-design/screens/amp-envelope-lfo.md "LFO"; UID-732…739; LFO-001…013). Template rail+content,
// context bar trackContext, breadcrumb HOME › TRACK MENU › LFO. Rail DESTINATION (OptionSelector, value = the target's
// name), SHAPE (OptionSelector), SETTING (Button), SYNC (Toggle, lfo.sync); body LfoGraph; strip SPEED, DEPTH,
// PHASE OFFSET (the 1.60 columns 1–3, CC 60–62) and DESTINATION, SHAPE mirroring the rail (UID-737), column 6 empty.
//   - DESTINATION popup: generated from ParamRegistry::lfoTargets() (ui/screens/lfo/Targets.h: NONE first, groups
//     PLAYBACK, AMP ENVELOPE, FX1 : <effect>, FX2 : <effect>, MIXER with non-focusable headers, the current row marked
//     and focused, SHIFT + turn jumps by group — the shell's ListPopup); column 4 walks the same list.
//   - A destination whose effect left its slot (LFO-013, UID-739): the stored name in text-3 with ⚠ (warn), rail and
//     column; the engine applies nothing (its ModSink finds no such parameter in the slot).
//   - SHAPE popup: the seven 1.60 entries with a 24 lpx glyph each (accent on the current row, UID-735).
//   - SETTING popup: Mode, Trigger On, Random Start (UID-736).
//   - SYNC on: SPEED shows the division (core::lfo::kSyncDivisions) and column 1 steps division by division; the window
//     is 2 bars; off: Hz over a 4 s window.
//   - Step held: the locked SPEED / DEPTH / PHASE waveform as a mod dashed overlay (the shell outlines the columns).
//   - NONE: waveform at 40 % + NO DESTINATION; TRACK MENU BYPASS (lfo.bypass): 40 % + BYPASS badge.
//   - Live value (UID-738) while the transport runs. engine/EngineApi.h's PlayheadFeed (frozen) carries no LFO
//     value and the UI may not include engine/modulation (tools/check-deps.py), so the dot is the UI's reconstruction
//     with the engine's laws: FREE and SYNC restart at the transport start (exact in SYNC, from the PPQ; free from the
//     UI clock), TRIGGER / ONE SHOT also at the track's hits as the shell's feeds detect them (EngineFeeds::lastHitMs,
//     ≈ one UI frame late). Random's held value follows the same xorshift32 sequence. The exact path needs a
//     per-track LFO value in the playhead snapshot (an amendment of the engine contract).
// Keyboard / panel: the rotary walks the rail (focus starts on DESTINATION), push opens the focused item's popup or
// toggles SYNC; encoders 1–5 the columns.
#include "core/util/lfo/LfoLaw.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/lfo/IconList.h"
#include "ui/screens/lfo/Targets.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/LfoGraph.h"
#include "ui/widgets/OptionSelector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace lfo = ::core::lfo;
constexpr int kRailDestination = 0, kRailShape = 1, kRailSetting = 2;
constexpr int kColSpeed = 0, kColDestination = 3;
constexpr int kModeFree = 0, kModeOneShot = 2;

// A 24 lpx drawing of a shape (SHAPE popup icon column): 12 columns of 2 lpx.
void shapeIcon(DrawList& dl, int shape, float x, float y, std::uint32_t ink)
{
    float prev = 0;
    for (int i = 0; i <= 12; ++i)
    {
        const double phase = shape == 6 ? static_cast<double>(i) / 3.0 : static_cast<double>(i) / 12.0; // RANDOM: 4 steps
        const float v = LfoGraph::shapeValue(shape, phase, shape == 6 ? 7u : 1u);
        const float yy = y + 12 - v * 9;
        if (i > 0)
        {
            const float top = std::min(prev, yy) - 1, bottom = std::max(prev, yy) + 1;
            dl.fill({x + 2.0f * static_cast<float>(i - 1), top, 2, bottom - top}, ink);
        }
        prev = yy;
    }
}

class LfoBody final : public Widget, public RailOwner, public StripOwner
{
public:
    explicit LfoBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return LfoGraph::kPanel; }

    const LfoGraph& graph() const noexcept { return graph_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        if (! focusSet_) // the rail holds the focus on entry (mockup lfo.svg: DESTINATION)
        {
            focusSet_ = true;
            if (ctx.focus.focused < 0)
                ctx.focus.focused = kRailDestination;
        }
        update(ctx.nowMs);
        graph_.build(dl, ctx);
    }

    bool dirty(double) const override { return graph_.liveOn(); }

    // --- rail ---
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        if (index == kRailDestination)
            st.value = destinationName();
    }
    bool railDraw(int index, const RailItemSpec& item, const RailItemState& st, DrawList& dl,
                  const UiContext& ctx) const override
    {
        if (index != kRailDestination || ! stale())
            return false;
        const auto r = L::railItem(index);
        OptionSelector::draw(dl, ctx, r, {item.label, {}, st.disabled, st.pressed, st.focused});
        const auto& p = ctx.palette;
        dl.text(r.x + tk::space[5], r.y + r.h - 30, Align::Left, TextStyleId::BodyStrong, p.text3, st.value);
        glyph::draw(dl, glyph::Kind::Caution, r.x + r.w - 64, r.y + r.h - 40, 20, p.warn);
        return true;
    }
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        switch (index)
        {
        case kRailDestination:
            openDestination();
            return true;
        case kRailShape:
            openShape();
            return true;
        case kRailSetting:
            openSetting();
            return true;
        default:
            return false; // SYNC: the shell toggles lfo.sync
        }
    }

    // --- strip ---
    void stripColumn(int column, ParamColumnModel& c) const override
    {
        if (shell_ == nullptr || state_ == nullptr || c.spec == nullptr)
            return;
        if (column == kColSpeed && sync())
            c.valueText = lfo::kSyncDivisions[static_cast<std::size_t>(lfo::syncDivisionIndex(c.value))].label;
        else if (column == kColDestination)
        {
            c.valueText = destinationName();
            c.stepped = true;
            c.inactive = stale();
        }
    }
    bool stripTurn(int column, const core::ParamSpec& spec, int detents, bool) override
    {
        if (shell_ == nullptr || state_ == nullptr || state_->stepHeld > 0)
            return false; // a held step: the shell's lock write
        const int t = state_->selectedTrack;
        if (column == kColSpeed && sync())
        {
            const int i = lfo::syncDivisionIndex(m1::trackParam(*shell_, spec.id, t, spec.defaultValue));
            m1::setTrackParam(*shell_, spec.id, t, lfo::syncDivisionKnob(std::clamp(i + detents, 0,
                                                                                    lfo::kNumSyncDivisions - 1)));
            return true;
        }
        if (column == kColDestination)
        {
            refreshTargets();
            const int now = currentIndex();
            const int next = std::clamp(now + detents, 0, static_cast<int>(targets_.size()) - 1);
            m1::setTrackParam(*shell_, "lfo.destination", t,
                              static_cast<float>(targets_[static_cast<std::size_t>(next)].value));
            return true;
        }
        return false;
    }

private:
    float param(const char* id, float fallback) const
    {
        return m1::trackParam(*shell_, id, state_->selectedTrack, fallback);
    }
    bool sync() const { return param("lfo.sync", 0) >= 0.5f; }
    core::ParamId destination() const
    {
        const float v = param("lfo.destination", -1);
        return v < 0 ? core::ParamId{} : core::ParamId{static_cast<std::uint16_t>(std::lround(v))};
    }
    std::string_view destinationName() const { return m4::targetName(shell_->params(), destination()); }
    bool stale() const
    {
        return m4::targetStale(shell_->params(), destination(), state_->scene - 1, state_->selectedTrack - 1);
    }
    double bpm() const
    {
        const auto& ph = m1::feedsOf(shell_).playhead();
        if (m1::feedsOf(shell_).live() && ph.bpm > 0)
            return ph.bpm;
        return std::max(20.0, std::strtod(state_->bpm.text, nullptr));
    }

    // The view of one set of SPEED / DEPTH / PHASE values (the base or a held step's locks).
    LfoView viewOf(float speed, float depth, float phase, int shape, bool syncOn, int randomStart, int mode) const
    {
        LfoView v;
        v.shape = shape;
        v.depth01 = lfo::depth01(depth);
        v.phase01 = lfo::phase01(phase);
        v.cycles = syncOn ? static_cast<float>(static_cast<double>(LfoGraph::kWindowQuarters) /
                                               lfo::kSyncDivisions[static_cast<std::size_t>(lfo::syncDivisionIndex(speed))]
                                                   .quarters)
                          : lfo::speedHz(speed) * LfoGraph::kWindowSeconds;
        v.seed = lfo::seedOf(randomStart);
        v.oneShot = mode == kModeOneShot;
        return v;
    }

    void update(double nowMs) const
    {
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        const float speed = param("lfo.speed", 21), depth = param("lfo.depth", 63), phase = param("lfo.phase", 0);
        const int shape = static_cast<int>(std::lround(param("lfo.shape", 0)));
        const int mode = static_cast<int>(std::lround(param("lfo.triggerMode", 1)));
        const int random = static_cast<int>(std::lround(param("lfo.randomStart", 0)));
        const bool syncOn = sync();
        const auto base = viewOf(speed, depth, phase, shape, syncOn, random, mode);
        graph_.setShape(base.shape, base.depth01, base.phase01, syncOn, bpm(), base.cycles, base.seed, base.oneShot);
        graph_.setStyle(m1::padInk(sh, t), static_cast<int>(std::lround(depth)),
                        ! destination().valid(), param("lfo.bypass", 0) >= 0.5f);
        // Step held: the locked waveform (UID-166 / amp-envelope-lfo.md "Step held").
        bool locked = false;
        float lv[3] = {speed, depth, phase};
        if (state_->stepHeld > 0)
        {
            static constexpr const char* ids[3] = {"lfo.speed", "lfo.depth", "lfo.phase"};
            for (int k = 0; k < 3; ++k)
                if (const auto* spec = sh.params().find(ids[k]))
                    locked |= sh.values().lockOf(*spec, state_->trackScope(), state_->stepHeld, lv[k]);
        }
        if (locked)
        {
            const auto v = viewOf(lv[0], lv[1], lv[2], shape, syncOn, random, mode);
            graph_.setLocked(&v);
        }
        else
            graph_.setLocked(nullptr);
        live(nowMs, base, syncOn, speed, mode);
    }

    // The live dot (see the file comment: the UI's reconstruction of the engine's LFO phase).
    void live(double nowMs, const LfoView& v, bool syncOn, float speed, int mode) const
    {
        const auto& feeds = m1::feedsOf(shell_);
        const auto& ph = feeds.playhead();
        const bool playing = feeds.live() ? ph.playing : state_->playing;
        const double quarters =
            lfo::kSyncDivisions[static_cast<std::size_t>(lfo::syncDivisionIndex(speed))].quarters;
        const double b = bpm();
        if (playing && ! wasPlaying_)
        {
            anchorPpq_ = ph.ppq;
            anchorMs_ = nowMs;
        }
        wasPlaying_ = playing;
        const double hit = feeds.lastHitMs(state_->selectedTrack);
        if (mode != kModeFree && hit != lastHit_ && hit > anchorMs_ && hit <= nowMs)
        {
            anchorMs_ = hit;
            anchorPpq_ = ph.ppq - (nowMs - hit) / 60000.0 * b;
        }
        lastHit_ = hit;
        if (! playing || v.cycles <= 0)
        {
            graph_.setLive(0, 0, false);
            return;
        }
        double e = syncOn ? (ph.ppq - anchorPpq_) / quarters
                          : static_cast<double>(lfo::speedHz(speed)) * (nowMs - anchorMs_) / 1000.0;
        e = std::max(0.0, e);
        if (v.oneShot)
            e = std::min(e, 1.0);
        const double inWindow = std::fmod(e, static_cast<double>(v.cycles));
        graph_.setLive(static_cast<float>(inWindow / static_cast<double>(v.cycles)),
                       LfoGraph::shapeValue(v.shape, static_cast<double>(v.phase01) + e, v.seed), true);
    }

    // The generated destination list of the selected track (rebuilt when the track, scene or an FX slot changes).
    void refreshTargets() const
    {
        const auto changes = ui::fx::environment().slotAccess().changes();
        const int key = (state_->scene - 1) * 16 + state_->selectedTrack - 1;
        if (! targets_.empty() && key == targetsKey_ && changes == targetsChanges_)
            return;
        m4::buildTargets(shell_->params(), m4::TargetKind::Lfo, state_->scene - 1, state_->selectedTrack - 1,
                         targets_);
        targetsKey_ = key;
        targetsChanges_ = changes;
    }
    int currentIndex() const
    {
        const auto d = destination();
        const int want = d.valid() ? static_cast<int>(d.value) : -1;
        for (std::size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].value == want)
                return static_cast<int>(i);
        return 0;
    }

    void openDestination()
    {
        refreshTargets();
        ListRequest req;
        req.title = "DESTINATION";
        req.items = targets_;
        req.grouped = true;
        req.current = currentIndex();
        const int t = state_->selectedTrack;
        req.onChoose = [this, t](int, const ListItem& it)
        { m1::setTrackParam(*shell_, "lfo.destination", t, static_cast<float>(it.value)); };
        shell_->showList(std::move(req));
    }

    void openShape()
    {
        const auto* spec = shell_->params().find("lfo.shape");
        if (spec == nullptr)
            return;
        ListRequest req;
        req.title = "SHAPE";
        for (std::size_t k = 0; k < spec->enumLabels.size(); ++k)
            req.items.push_back({spec->enumLabels[k], {}, "shape", static_cast<int>(k)});
        req.current = static_cast<int>(std::lround(param("lfo.shape", 0)));
        const int t = state_->selectedTrack;
        req.onChoose = [this, t](int, const ListItem& it)
        { m1::setTrackParam(*shell_, "lfo.shape", t, static_cast<float>(it.value)); };
        shell_->showPopup(std::make_unique<m4::IconListPopup>(
            std::move(req), [](DrawList& dl, int item, float x, float y, std::uint32_t ink)
            { shapeIcon(dl, item, x, y, ink); }));
    }

    void openSetting()
    {
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        SettingRequest req;
        req.title = "SETTING";
        static constexpr const char* ids[] = {"lfo.triggerMode", "lfo.triggerOn", "lfo.randomStart"};
        static constexpr std::string_view labels[] = {"Mode", "Trigger On", "Random Start"};
        for (int i = 0; i < 3; ++i)
        {
            SettingRow row;
            row.label = labels[i];
            const auto* spec = sh.params().find(ids[i]);
            const float v = m1::trackParam(sh, ids[i], t);
            if (spec != nullptr && spec->type == core::ParamType::Enum)
            {
                row.options.assign(spec->enumLabels.begin(), spec->enumLabels.end());
                row.current = static_cast<int>(std::lround(v - spec->minValue));
            }
            else
            {
                row.spec = spec;
                row.value = v;
            }
            req.rows.push_back(std::move(row));
        }
        req.onCommit = [this, t](int row, int option, float value)
        {
            if (row == 0 || row == 1)
                m1::setTrackParam(*shell_, ids[row], t, static_cast<float>(option));
            else if (row == 2)
                m1::setTrackParam(*shell_, ids[row], t, value);
        };
        sh.showSetting(std::move(req));
    }

    ShellApi* shell_;
    UiState* state_;
    mutable LfoGraph graph_{};
    mutable bool focusSet_ = false, wasPlaying_ = false;
    mutable double anchorPpq_ = 0, anchorMs_ = 0, lastHit_ = -1e9;
    mutable std::vector<ListItem> targets_;
    mutable int targetsKey_ = -1;
    mutable std::uint32_t targetsChanges_ = 0;
};

std::unique_ptr<Widget> makeLfoGraph(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<LfoBody>(a);
}

constexpr RailItemSpec kLfoRail[] = {{RailItemSpec::Kind::Option, "DESTINATION", {}, "lfo.destination"},
                                     {RailItemSpec::Kind::Option, "SHAPE", {}, "lfo.shape"},
                                     {RailItemSpec::Kind::Button, "SETTING"},
                                     {RailItemSpec::Kind::Toggle, "SYNC", {}, "lfo.sync"}};
} // namespace

// Test / bench access: the LFO body's graph (null for another body).
const LfoGraph* lfoGraphOf(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const LfoBody*>(body);
    return b != nullptr ? &b->graph() : nullptr;
}
} // namespace ui

APP_REGISTER_WIDGET("LfoGraph", &ui::makeLfoGraph)
APP_REGISTER_SCREEN((ui::ScreenSpec{
    .id = "track.lfo",
    .title = "LFO",
    .parent = "track.menu",
    .body = ui::BodyTemplate::RailContent,
    .contextBar = ui::ContextBarKind::TrackContext,
    .rail = ui::kLfoRail,
    .bodyComponent = "LfoGraph",
    .strip = {"LFO", {"lfo.speed", "lfo.depth", "lfo.phase", "lfo.destination", "lfo.shape", ""}}}))
