#include "ui/widgets/FxGrid.h"

#include "ui/screens/effects/FxCatalogue.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/EffectIcon.h"
#include "ui/widgets/EffectPanel.h"
#include "ui/widgets/GridPopup.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui
{
namespace
{
// Popup-relative geometry of the mockup (_tools/screens/effects.py select_fx, popup at 48, 48): the note after the
// title, the pager.
constexpr float kNoteX = 136, kNoteY = 29, kPagerY = 636, kPrevX = 1000, kDotsX = 1032, kCountX = 1144;

struct GridState
{
    std::vector<const engine::fx::EffectDescriptor*> list;
    std::string note; // "FX1 · TRACK 5"
    FxGrid::OnChoose onChoose;
};
} // namespace

std::string_view FxGrid::labelOf(const engine::fx::EffectDescriptor& d) noexcept
{
    if (const auto* h = fx::environment().hintsRegistry().find(d.id); h != nullptr && ! h->gridLabel.empty())
        return h->gridLabel;
    return d.name;
}

GridRequest FxGrid::request(fx::Slot slot, int track, std::string_view currentId, OnChoose onChoose)
{
    auto state = std::make_shared<GridState>();
    const auto& env = fx::environment();
    fx::listFor(env.effectRegistry(), fx::kind(slot), env.flags, state->list);
    char note[48];
    if (fx::isInsert(slot))
        std::snprintf(note, sizeof note, "%.*s \xC2\xB7 TRACK %d", static_cast<int>(fx::tab(slot).size()),
                      fx::tab(slot).data(), track);
    else
        std::snprintf(note, sizeof note, "%.*s", static_cast<int>(fx::tab(slot).size()), fx::tab(slot).data());
    state->note = note;
    state->onChoose = std::move(onChoose);

    GridRequest r;
    r.title = "SELECT FX";
    for (std::size_t i = 0; i < state->list.size(); ++i)
    {
        const auto* d = state->list[i];
        r.items.push_back({labelOf(*d), iconIdFor(d->id), true});
        if (d->id == currentId || (fx::isNone(currentId) && d->id == fx::kNone))
            r.current = static_cast<int>(i);
    }
    r.drawCell = [state](DrawList& dl, const GridCell& c)
    {
        EffectIcon::draw(dl, c.icon, c.icon48.x, c.icon48.y, 48, c.ink);
        if (c.item < 0 || c.item >= static_cast<int>(state->list.size()))
            return;
        const auto& p = c.palette != nullptr ? *c.palette : theme::palette(true);
        const float cx = c.cell.x, cy = c.cell.y;
        if (fx::isExtension(*state->list[static_cast<std::size_t>(c.item)]))
        {
            dl.fill({cx + 12, cy + 10, 40, 22}, p.surface3, 4);
            dl.text(cx + 32, cy + 21, Align::Centre, TextStyleId::Caption, p.text2, "EXT");
        }
        if (c.slotOnPage != 0)
            return;
        // Once per page (drawn with its first cell): the note after the title and the pager.
        const float bx = c.popup.x, by = c.popup.y; // the popup's top-left (follows its open animation)
        dl.text(bx + kNoteX, by + kNoteY, Align::Left, TextStyleId::Caption, p.text2, state->note);
        const int pages = c.pages;
        if (pages > 1)
        {
            const int page = c.page + 1;
            char count[32];
            std::snprintf(count, sizeof count, "%d / %d", page, pages);
            dl.text(bx + kPrevX, by + kPagerY, Align::Centre, TextStyleId::Title, p.text2, "\xE2\x80\xB9");
            dl.text(bx + kDotsX + 16 * static_cast<float>(pages) - 8, by + kPagerY, Align::Centre, TextStyleId::Title,
                    p.text2, "\xE2\x80\xBA");
            dl.text(bx + kCountX, by + kPagerY, Align::Right, TextStyleId::Caption, p.text2, count);
        }
    };
    r.onChoose = [state](int index)
    {
        if (index >= 0 && index < static_cast<int>(state->list.size()) && state->onChoose)
            state->onChoose(*state->list[static_cast<std::size_t>(index)]);
    };
    return r;
}
} // namespace ui
