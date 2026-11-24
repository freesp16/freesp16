#include "ui/widgets/PianoKeyboard.h"

#include "ui/theme/Draw.h"

#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr int kWhiteOf[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6}; // semitone → white key index (−1 = black)
constexpr int kSemitoneOfWhite[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr const char* kWhiteNames[7] = {"C", "D", "E", "F", "G", "A", "B"};
constexpr int kBlackAfter[5] = {0, 1, 3, 4, 5}; // white keys followed by a black key
constexpr float kBandY = 152, kKeyY = 192, kWhiteW = 84, kWhiteH = 296, kBlackW = 52, kBlackH = 184;
constexpr float kOvX = 80, kOvY = 524, kOvKey = 16;

// The MIDI note → octave with C0 = 0 (the manual's C0…B9, the catalogue's naming: 60 = C5).
int octaveOf(int note) noexcept
{
    return note / 12;
}
} // namespace

void PianoKeyboard::draw(DrawList& dl, const UiContext& ctx, const PianoKeyboardModel& m)
{
    const auto& p = ctx.palette;
    char text[8];
    for (int band = 0; band < 2; ++band)
    {
        const int octave = m.firstOctave + band;
        const float ox = 16 + 632 * static_cast<float>(band);
        dl.fill({ox, kBandY, 616, 32}, p.surface2, tk::radiusS);
        std::snprintf(text, sizeof text, "C%d", octave);
        dl.text(ox + 12, 168, Align::Left, TextStyleId::Label, p.text2, text);
        const int base = octave * 12;
        for (int i = 0; i < 7; ++i)
        {
            const int note = base + kSemitoneOfWhite[i];
            int mark = -1;
            for (int k = 0; k < 4; ++k)
                if (m.notes[static_cast<std::size_t>(k)] == note && mark < 0)
                    mark = k;
            const float x = ox + 88 * static_cast<float>(i) + 2;
            const auto fill = mark < 0 ? p.text1 : (mark == 0 ? m.colour : theme::withOpacity(m.colour, 0.6f));
            dl.fill({x, kKeyY, kWhiteW, kWhiteH}, fill, tk::radiusS);
            std::snprintf(text, sizeof text, "%s%d", kWhiteNames[i], octave);
            dl.text(x + 42, 470, Align::Centre, TextStyleId::Caption, p.onAccent, text);
            if (mark >= 0)
            {
                dl.fill({x + 30, 412, 24, 24}, p.bg, tk::radiusXs);
                std::snprintf(text, sizeof text, "%d", mark + 1);
                dl.text(x + 42, 425, Align::Centre, TextStyleId::Label, p.text1, text);
            }
            if (mark == 0 && m.locked)
                dl.stroke({x - 1, kKeyY - 1, kWhiteW + 2, kWhiteH + 2}, p.mod, 1.5f, tk::radiusS);
            if (note == m.focusedNote)
                draw::focusRing(dl, p, {x, kKeyY, kWhiteW, kWhiteH}, tk::radiusS);
        }
        for (const int i : kBlackAfter)
        {
            const int note = base + kSemitoneOfWhite[i] + 1;
            int mark = -1;
            for (int k = 0; k < 4; ++k)
                if (m.notes[static_cast<std::size_t>(k)] == note && mark < 0)
                    mark = k;
            const RectF r {ox + 88 * static_cast<float>(i + 1) - 26, kKeyY, kBlackW, kBlackH};
            dl.fill(r, mark < 0 ? p.surface3 : (mark == 0 ? m.colour : theme::withOpacity(m.colour, 0.6f)), tk::radiusXs);
            if (mark >= 0)
            {
                std::snprintf(text, sizeof text, "%d", mark + 1);
                dl.text(r.x + r.w / 2, r.bottom() - 18, Align::Centre, TextStyleId::Label, p.text1, text);
            }
            if (note == m.focusedNote)
                draw::focusRing(dl, p, r, tk::radiusXs);
        }
    }
    // The overview keyboard C0–B9 with the shown octaves framed (‹ › move the window).
    for (int o = 0; o < 10; ++o)
    {
        std::snprintf(text, sizeof text, "C%d", o);
        dl.text(kOvX + static_cast<float>(o) * 7 * kOvKey + 2, 512, Align::Left, TextStyleId::Caption, p.text3, text);
    }
    const int note = m.notes[0];
    const int noteWhite = note >= 0 && kWhiteOf[note % 12] >= 0 ? octaveOf(note) * 7 + kWhiteOf[note % 12] : -1;
    const auto whiteInk = theme::withOpacity(p.text1, 0.85f);
    for (int i = 0; i < 70; ++i)
        dl.fill({kOvX + static_cast<float>(i) * kOvKey, kOvY, kOvKey - 2, 44}, i == noteWhite ? m.colour : whiteInk, 2);
    for (int i = 0; i < 70; ++i)
        if (const int w = i % 7; w == 0 || w == 1 || w == 3 || w == 4 || w == 5)
        {
            const bool current = note >= 0 && kWhiteOf[note % 12] < 0 && octaveOf(note) * 7 + kWhiteOf[(note - 1) % 12] == i;
            dl.fill({kOvX + static_cast<float>(i + 1) * kOvKey - 6, kOvY, 10, 26}, current ? m.colour : p.surface3, 1);
        }
    const float fx = kOvX + static_cast<float>(m.firstOctave * 7) * kOvKey;
    dl.stroke({fx - 4, kOvY - 6, 14 * kOvKey + 6, 56}, p.text1, 2, tk::radiusXs);
    dl.text(fx - 16, 592, Align::Centre, TextStyleId::Title, p.text2, "\xE2\x80\xB9");
    dl.text(fx + 14 * kOvKey + 12, 592, Align::Centre, TextStyleId::Title, p.text2, "\xE2\x80\xBA");
}

int PianoKeyboard::noteAt(const PianoKeyboardModel& m, float x, float y) noexcept
{
    if (y < kKeyY || y >= kKeyY + kWhiteH)
        return -1;
    for (int band = 0; band < 2; ++band)
    {
        const float ox = 16 + 632 * static_cast<float>(band);
        const int base = (m.firstOctave + band) * 12;
        if (y < kKeyY + kBlackH)
            for (const int i : kBlackAfter)
            {
                const float bx = ox + 88 * static_cast<float>(i + 1) - 26;
                if (x >= bx && x < bx + kBlackW)
                    return base + kSemitoneOfWhite[i] + 1;
            }
        for (int i = 0; i < 7; ++i)
        {
            const float wx = ox + 88 * static_cast<float>(i) + 2;
            if (x >= wx && x < wx + kWhiteW)
                return base + kSemitoneOfWhite[i];
        }
    }
    return -1;
}
} // namespace ui
