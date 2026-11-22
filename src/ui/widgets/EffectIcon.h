// EffectIcon (07-ui-design/components/visuals.md "Icons" — Effects, 48 lpx variants; "ModuleCard" thumbnails;
// UID-400; UI-261): the effect icon family — NONE (⊘) and the twelve reference effects plus EXT for any effect
// without its own drawing — as the app's own line drawings on the 24 lpx grid (round caps, no fills except where
// noted). No vendor artwork: every icon is drawn from the spec's one-line description ("filter: low-pass curve" …).
//
// The drawings are data (IconPart lists below, in SVG path syntax: M L H V C Z, circles, discs, arcs). The published
// files resources/icons/fx/<id>.svg are exactly effectIconSvg(id) (a unit test keeps them identical). At start-up the
// family is flattened once into an icon atlas of draw primitives at 24, 40 and 48 lpx (the DrawList has rounded
// rectangles only, ui/theme/Glyphs.h): drawing an icon copies its cached primitives — no parsing, no trigonometry and
// no allocation per frame (SELECT FX opens ≤ 2 ms).
#pragma once

#include "ui/screens/effects/FxSlots.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/Widget.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ui
{
class ShellApi;

struct IconPart
{
    enum class Kind : std::uint8_t
    {
        Path,
        Circle,
        Disc,
        Arc
    } kind;
    std::string_view d{};        // Path: absolute M L H V C Z commands on the 24 grid
    float cx = 0, cy = 0, r = 0; // Circle / Disc / Arc
    float a0 = 0, a1 = 0;        // Arc: start / end angle in degrees (0 = 3 o'clock, clockwise)
    float opacity = 1;
};

struct IconDef
{
    std::string_view id;
    std::span<const IconPart> parts;
};

std::span<const IconDef> effectIcons() noexcept;
const IconDef* findEffectIcon(std::string_view id) noexcept; // exact id, or null
// The icon id an effect draws with: EffectUiSpec::icon, else the effect id when the family has it, else "ext"
// (NONE for an empty id).
std::string_view iconIdFor(std::string_view effectId) noexcept;
// The published SVG of an icon (48 × 48, viewBox 0 0 24 24, stroke currentColor).
std::string effectIconSvg(const IconDef&);

class EffectIcon final : public Widget
{
public:
    // Draws icon `id` (unknown → EXT) with its 24-grid origin at (x, y), `size` lpx wide (24, 40 or 48 are cached;
    // other sizes fall back to the nearest cached size, scaled), in `ink` (part opacity multiplies its alpha).
    static void draw(DrawList&, std::string_view id, float x, float y, float size, std::uint32_t ink);
    static void warm(); // builds the atlas (start-up; draw() also builds it on first use)
    static std::size_t primitives(std::string_view id, float size); // cached primitive count (tests, bench)

    // The TRACK MENU FX card thumbnail (UI-261): the current effect of `slot` for the selected track, text-2, 48 lpx
    // when the bounds are ≥ 48 tall, else 24, centred.
    EffectIcon(RectF bounds, UiState* state, fx::Slot slot) noexcept : bounds_(bounds), state_(state), slot_(slot) {}
    RectF bounds() const noexcept override { return bounds_; }
    void build(DrawList&, const UiContext&) const override;

private:
    RectF bounds_;
    UiState* state_;
    fx::Slot slot_;
};
} // namespace ui
