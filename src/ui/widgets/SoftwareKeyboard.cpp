#include "ui/widgets/SoftwareKeyboard.h"

#include "ui/shell/Layout.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/Button.h"

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;

constexpr std::string_view kRow0 = "qwertyuiop", kRow1 = "asdfghjkl", kRow2 = "zxcvbnm";
constexpr std::string_view kRow0Upper = "QWERTYUIOP", kRow1Upper = "ASDFGHJKL", kRow2Upper = "ZXCVBNM";
constexpr std::string_view kDigits = "1234567890", kSymbols = "()&@#+,'!";
constexpr std::string_view kRow2Tail = "-_.";

struct Slot
{
    int row, col;
    float width;
};
// Index → (row, column, width). Row 0: 10 + ⌫ · row 1: 9 + DONE · row 2: ⇧ + 7 + 3 · row 3: 123 SPACE ← →.
Slot slotOf(int i) noexcept
{
    if (i < 11)
        return {0, i, 104};
    if (i < 21)
        return {1, i - 11, i == 20 ? 216.0f : 104.0f};
    if (i < 32)
        return {2, i - 21, 104};
    static constexpr float row3[] = {160, 704, 160, 160};
    return {3, i - 32, row3[i - 32]};
}
float rowWidth(int row) noexcept
{
    float w = 0;
    int n = 0;
    for (int i = 0; i < SoftwareKeyboard::kKeyCount; ++i)
        if (slotOf(i).row == row)
        {
            w += slotOf(i).width;
            ++n;
        }
    return w + L::keyGap * static_cast<float>(n - 1);
}
} // namespace

RectF SoftwareKeyboard::bounds() const noexcept
{
    return L::keyboard;
}

SoftwareKeyboard::Key SoftwareKeyboard::key(int i) const noexcept
{
    Key k;
    if (i < 0 || i >= kKeyCount)
        return k;
    const auto s = slotOf(i);
    float x = (L::canvasW - rowWidth(s.row)) / 2;
    for (int j = 0; j < i; ++j)
        if (slotOf(j).row == s.row)
            x += slotOf(j).width + L::keyGap;
    k.rect = {x, L::keyRow0 + L::keyRowPitch * static_cast<float>(s.row), s.width, L::keyH};
    auto one = [](std::string_view row, int c) { return row.substr(static_cast<std::size_t>(c), 1); };
    switch (s.row)
    {
    case 0:
        if (s.col == 10)
            k.kind = KeyKind::Backspace;
        else
            k.text = digits ? one(kDigits, s.col) : one(shift ? kRow0Upper : kRow0, s.col);
        break;
    case 1:
        if (s.col == 9)
            k.kind = KeyKind::Done;
        else
            k.text = digits ? one(kSymbols, s.col) : one(shift ? kRow1Upper : kRow1, s.col);
        break;
    case 2:
        if (s.col == 0)
            k.kind = KeyKind::Shift;
        else if (s.col <= 7)
            k.text = one(shift ? kRow2Upper : kRow2, s.col - 1);
        else
            k.text = one(kRow2Tail, s.col - 8);
        break;
    default:
    {
        static constexpr KeyKind row3[] = {KeyKind::Layer, KeyKind::Space, KeyKind::Left, KeyKind::Right};
        k.kind = row3[s.col];
        if (k.kind == KeyKind::Space)
            k.text = " ";
        break;
    }
    }
    return k;
}

int SoftwareKeyboard::keyAt(float x, float y) const noexcept
{
    for (int i = 0; i < kKeyCount; ++i)
    {
        const auto r = key(i).rect;
        // Hit areas extend into the 8 lpx gaps so every point of a row hits a key.
        if (x >= r.x - L::keyGap / 2 && x < r.x + r.w + L::keyGap / 2 && y >= r.y - L::keyGap / 2 &&
            y < r.y + r.h + L::keyGap / 2)
            return i;
    }
    return -1;
}

std::string_view SoftwareKeyboard::press(int i) noexcept
{
    const auto k = key(i);
    switch (k.kind)
    {
    case KeyKind::Shift:
        shift = ! shift;
        return {};
    case KeyKind::Layer:
        digits = ! digits;
        return {};
    case KeyKind::Char:
    case KeyKind::Space:
        return k.text;
    default:
        return {};
    }
}

void SoftwareKeyboard::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    dl.fill(L::keyboard, p.surface1);
    draw::hline(dl, 0, L::canvasW, L::keyboard.y, p.border);
    const bool pressedLive = ctx.nowMs - pressedAtMs < tk::motionFastMs;
    for (int i = 0; i < kKeyCount; ++i)
    {
        const auto k = key(i);
        const auto& r = k.rect;
        const bool pressed = pressedLive && pressedKey == i;
        const float cx = r.x + r.w / 2, cy = r.y + 27;
        if (k.kind == KeyKind::Done)
        {
            Button::draw(dl, ctx, r, {doneLabel, ButtonStyle::Primary, ! doneEnabled, pressed, false});
            continue;
        }
        const bool special = k.kind != KeyKind::Char;
        dl.fill(r, pressed ? p.borderStrong : (special ? p.surface3 : p.surface2), tk::radiusS);
        const float g = tk::sizeIcon;
        switch (k.kind)
        {
        case KeyKind::Char:
            dl.text(cx, cy, Align::Centre, TextStyleId::Title, p.text1, k.text);
            break;
        case KeyKind::Backspace:
            glyph::draw(dl, glyph::Kind::Backspace, cx - g / 2, cy - g / 2, g, p.text2);
            break;
        case KeyKind::Shift:
            glyph::draw(dl, glyph::Kind::Shift, cx - g / 2, cy - g / 2, g, shift ? p.accent : p.text2);
            break;
        case KeyKind::Left:
            glyph::draw(dl, glyph::Kind::ArrowLeft, cx - g / 2, cy - g / 2, g, p.text2);
            break;
        case KeyKind::Right:
            glyph::draw(dl, glyph::Kind::ArrowRight, cx - g / 2, cy - g / 2, g, p.text2);
            break;
        case KeyKind::Layer:
            dl.text(cx, cy, Align::Centre, TextStyleId::Label, p.text2, digits ? "ABC" : "123");
            break;
        case KeyKind::Space:
            dl.text(cx, cy, Align::Centre, TextStyleId::Label, p.text2, "SPACE");
            break;
        case KeyKind::Done:
            break;
        }
    }
}
} // namespace ui
