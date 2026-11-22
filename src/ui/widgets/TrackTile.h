// TrackTile (07-ui-design/screens/home.md, track-setting.md; UID-500…504, UID-525…531; UI-210…217, UI-410): one
// 278 × 102 tile of the 4 × 4 track grid in pad order. Header: the track chip (32 × 24, pad colour, `dim` when empty)
// and either the HOME status badges (type, stretch, loop × trigger glyph, ⚠ when the sample is missing, 5-segment
// mini level) or the TRACK SETTING badges (`VEL`, `CHOKE`, `#NN` in the pad colour). Body: the name, middle-truncated
// to the tile, or a 4 lpx accent progress bar while a sample loads. States: selected (2 lpx frame in the pad colour),
// focused (accent ring), muted (40 %), soloed (2 lpx warn top bar), trigger flash (pad colour 30 % decaying over
// motion.pad-flash), missing (⚠ + name in text-3). A flash never changes layout (UID-503).
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
struct TrackSummary;

struct TrackTileModel
{
    enum class Variant : std::uint8_t
    {
        Home,
        Setting
    };
    int track = 1;  // 1–16
    int colour = 0; // pad colour 1–16 (0 = the track's default colour)
    Variant variant = Variant::Home;
    const TrackSummary* summary = nullptr;
    bool selected = false, focused = false;
    float flash = 0;         // 0–1: the trigger flash's current strength
    float levelDb = -120.0f; // mini level (HOME)
    float loading = -1;      // 0–1 while a sample loads (UI-410), < 0 = not loading
    // TRACK SETTING badges (text views of static storage or of the caller's buffers).
    std::string_view velBadge{}, chokeBadge{}, colourBadge{};
};

class TrackTile
{
public:
    static void draw(DrawList&, const UiContext&, RectF bounds, const TrackTileModel&);
    // The number of lit mini-level segments (0–5) for a level in dB.
    static int levelSegments(float db) noexcept;
    // Copies `name` into `out` (capacity n) middle-truncated with "…" so it fits `maxW` lpx in `style`; returns the
    // view.
    static std::string_view middleTruncate(std::string_view name, float maxW, TextStyleId style,
                                           const TextMeasure& measure, char* out, std::size_t n);
};
} // namespace ui
