// SET SCALE and SET SCALE (MIDI) (07-ui-design/screens/playback.md "SET SCALE"; UID-711…714; PAD-010…013, PAD-020,
// MID-046; moved here from PlaybackScreen.cpp). Template rail+content, context bar
// trackContext. Rail SCALE (OptionSelector → the SCALE ListPopup, CHROMATIC first, current focused and ✓, each row
// with a 24 lpx mini degree strip: 12 ticks, in-scale ones filled text-2); body ScaleMap (the 4 × 16 degree map
// with the pads' window framed) + PadMap (4 × 4 pads in pad order).
//   Sample tracks  `track.playback.setScale` (HOME › TRACK MENU › PLAYBACK › SET SCALE): scale.type, pads labelled with
//                  their semitone offsets; strip AREA (knob 1), TIME STRETCH (2), KEY SHIFT (3) (manual p.25–28).
//   MIDI / AS-1    `track.setScaleMidi` (HOME › TRACK MENU › SET SCALE, the TRACK MENU header action): scaleMidi.type,
//                  pads labelled with their note names from ROOT (core::scales::midiPadNote); strip AREA (knob 1),
//                  ROOT NOTE (knob 3, the manual's 1.4), no TIME STRETCH (SAMPLE tracks only, the manual's 7).
// The scale tables are the engine's (core/util/scales): scale.type = core::scales::kScaleMasks order.
// Keyboard / panel: the rotary reaches SCALE, push opens the list; encoders 1–3 the columns.
#include "core/util/scales/Scales.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/lfo/IconList.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/widgets/ScaleMap.h"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
namespace L = layout;

// 12 ticks of 2 lpx in a 24 lpx strip: in-scale degrees filled, the others outlined.
void degreeStrip(DrawList& dl, std::uint16_t mask, float x, float y, std::uint32_t ink)
{
    for (int i = 0; i < 12; ++i)
    {
        const RectF tick{x + 2.0f * static_cast<float>(i), y + 4, 1.5f, 16};
        if ((mask >> i) & 1u)
            dl.fill(tick, ink);
        else
            dl.fill({tick.x, tick.y + 12, tick.w, 4}, theme::withOpacity(ink, 0.5f));
    }
}

class SetScaleBody final : public Widget, public RailOwner
{
public:
    SetScaleBody(const WidgetArgs& a, bool midi) noexcept : shell_(a.shell), state_(a.state), midi_(midi) {}
    RectF bounds() const noexcept override { return {224, L::contentTop, 1040, L::contentH}; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        if (! focusSet_) // the rail's SCALE holds the focus on entry (the rotary has nothing else to reach)
        {
            focusSet_ = true;
            if (ctx.focus.focused < 0)
                ctx.focus.focused = 0;
        }
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        const auto col = theme::trackColour(m1::padColour(sh, t));
        const int type = std::clamp(static_cast<int>(std::lround(m1::trackParam(sh, typeId(), t, midi_ ? 0 : 2))), 0,
                                    core::scales::kScales - 1);
        const std::uint16_t mask = core::scales::scaleMask(type);
        const int area = std::clamp(static_cast<int>(std::lround(m1::trackParam(sh, areaId(), t))),
                                    core::scales::kMinWindow, core::scales::kMaxWindow);
        PadMapModel pm;
        pm.mask = mask;
        pm.area = area;
        pm.colour = col;
        pm.midi = midi_;
        if (midi_)
            pm.rootNote = static_cast<int>(std::lround(m1::trackParam(sh, "scaleMidi.root", t, 60)));
        else
            pm.keyShift = static_cast<int>(std::lround(m1::trackParam(sh, "scale.keyShift", t)));
        // The selected key (the manual's 1.6): the pitch the last SCALE pad played, wherever AREA / SCALE / ROOT
        // put it now (not shown when the scale lacks it); the root until a pad is pressed.
        drawnKey_ = state_->tracks[static_cast<std::size_t>((t - 1) & 15)].scaleKey;
        if (drawnKey_ != TrackSummary::kNoKey)
            pm.selectedDegree = core::scales::degreeOf(
                drawnKey_ - (midi_ ? std::clamp(pm.rootNote, 0, 127)
                                   : std::clamp(pm.keyShift, core::scales::kMinKeyShift, core::scales::kMaxKeyShift)),
                mask);
        ScaleMapModel sm;
        sm.mask = mask;
        sm.area = area;
        sm.colour = col;
        sm.selectedDegree = pm.selectedDegree;
        ScaleMap::draw(dl, ctx, sm);
        PadMap::draw(dl, ctx, pm);
    }
    bool dirty(double) const override // a SCALE pad press selects another key (the pads bypass the shell)
    {
        return state_ != nullptr && state_->tracks[static_cast<std::size_t>((state_->selectedTrack - 1) & 15)].scaleKey
                                        != drawnKey_;
    }

    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (index != 0 || shell_ == nullptr || state_ == nullptr)
            return false;
        const auto* spec = shell_->params().find(typeId());
        if (spec == nullptr)
            return false;
        ListRequest req;
        req.title = "SCALE";
        for (std::size_t k = 0; k < spec->enumLabels.size(); ++k)
            req.items.push_back({spec->enumLabels[k], {}, "degrees", static_cast<int>(k)});
        const int t = state_->selectedTrack;
        req.current = static_cast<int>(std::lround(m1::trackParam(*shell_, typeId(), t) - spec->minValue));
        const char* id = typeId();
        req.onChoose = [this, t, id](int, const ListItem& it)
        { m1::setTrackParam(*shell_, id, t, static_cast<float>(it.value)); };
        shell_->showPopup(std::make_unique<m4::IconListPopup>(
            std::move(req), [](DrawList& dl, int item, float x, float y, std::uint32_t ink)
            { degreeStrip(dl, core::scales::scaleMask(item), x, y, ink); }));
        return true;
    }

private:
    const char* typeId() const noexcept { return midi_ ? "scaleMidi.type" : "scale.type"; }
    const char* areaId() const noexcept { return midi_ ? "scaleMidi.area" : "scale.area"; }

    ShellApi* shell_;
    UiState* state_;
    bool midi_;
    mutable bool focusSet_ = false;
    mutable int drawnKey_ = TrackSummary::kNoKey;
};

std::unique_ptr<Widget> makeScaleMap(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<SetScaleBody>(a, s.id == "track.setScaleMidi");
}

constexpr RailItemSpec kScaleRail[] = {{RailItemSpec::Kind::Option, "SCALE", {}, "scale.type"}};
constexpr RailItemSpec kScaleMidiRail[] = {{RailItemSpec::Kind::Option, "SCALE", {}, "scaleMidi.type"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("ScaleMap", &ui::makeScaleMap)
APP_REGISTER_SCREEN((ui::ScreenSpec{
    .id = "track.playback.setScale",
    .title = "SET SCALE",
    .parent = "track.playback",
    .body = ui::BodyTemplate::RailContent,
    .contextBar = ui::ContextBarKind::TrackContext,
    .rail = ui::kScaleRail,
    .bodyComponent = "ScaleMap",
    .strip = {"SET SCALE", {"scale.area", "playback.timeStretch", "scale.keyShift", "", "", ""}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.setScaleMidi",
                                    .title = "SET SCALE",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = ui::kScaleMidiRail,
                                    .bodyComponent = "ScaleMap",
                                    .strip = {"SET SCALE", {"scaleMidi.area", "", "scaleMidi.root", "", "", ""}}}))
