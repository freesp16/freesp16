#include "ui/widgets/EffectIcon.h"

#include "ui/descriptors/EffectSpec.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ui
{
namespace
{
using K = IconPart::Kind;

// --- the family (own drawings from components/visuals.md "Icons", 24 lpx grid) --------------------------------------
constexpr IconPart kNone[] = {{K::Circle, {}, 12, 12, 9}, {K::Path, "M 5.6 18.4 L 18.4 5.6"}};
// delay: three fading circles, each smaller (the repeats die away)
constexpr IconPart kDelay[] = {{K::Circle, {}, 6.5, 12, 4.5},
                               {K::Circle, {}, 13.5, 12, 3.5, 0, 0, 0.6f},
                               {K::Circle, {}, 19.5, 12, 2.5, 0, 0, 0.35f}};
// reverb: a source and concentric arcs spreading to the right
constexpr IconPart kReverb[] = {{K::Disc, {}, 4.5, 12, 1.75},
                                {K::Arc, {}, 4.5, 12, 6, -48, 48, 0.9f},
                                {K::Arc, {}, 4.5, 12, 11, -45, 45, 0.6f},
                                {K::Arc, {}, 4.5, 12, 16, -40, 40, 0.35f}};
// chorus: three overlapping voices
constexpr IconPart kChorus[] = {
    {K::Circle, {}, 8, 12, 5.5}, {K::Circle, {}, 12, 12, 5.5, 0, 0, 0.7f}, {K::Circle, {}, 16, 12, 5.5, 0, 0, 0.45f}};
// flanger: three swept lines
constexpr IconPart kFlanger[] = {{K::Path, "M 2 20 C 8 20 13 12 22 9"},
                                 {K::Path, "M 2 15.5 C 8 15.5 13 8 22 5.5", {}, 0, 0, 0, 0, 0.65f},
                                 {K::Path, "M 2 11 C 8 11 13 4.5 22 2.5", {}, 0, 0, 0, 0, 0.35f}};
// phaser: a comb of notches, shallower to the right
constexpr IconPart kPhaser[] = {
    {K::Path, "M 2 6 H 3.5 C 5 6 5 19 6.5 19 C 8 19 8 6 9.5 6 H 10.5 C 12 6 12 16 13.5 16 C 15 16 15 6 16.5 6 "
              "H 17.5 C 19 6 19 12.5 20 12.5 C 21 12.5 21 6 22 6"}};
// eq2: a two-band response over the 0 dB line
constexpr IconPart kEq2[] = {{K::Path, "M 2 14 C 5 14 6 8.5 8.5 8.5 C 11 8.5 11 14 13 14 C 16 14 17 6.5 22 6.5"},
                             {K::Path, "M 2 20.5 H 22", {}, 0, 0, 0, 0, 0.4f}};
// filter: a low-pass curve with a resonant bump
constexpr IconPart kFilter[] = {{K::Path, "M 2 9 H 11 C 13.5 9 14.5 6.5 16 8 C 17.5 9.5 18.5 16 21.5 20"},
                                {K::Path, "M 2 21 H 22", {}, 0, 0, 0, 0, 0.4f}};
// lofi: a stepped (sample-and-hold) wave
constexpr IconPart kLofi[] = {{K::Path, "M 2 13 H 5 V 8 H 8 V 5.5 H 11 V 10 H 14 V 17.5 H 17 V 14 H 20 V 10 H 22"}};
// compressor: input/output axes and the knee curve
constexpr IconPart kCompressor[] = {{K::Path, "M 3 3 V 21 H 21", {}, 0, 0, 0, 0, 0.4f},
                                    {K::Path, "M 3.5 20.5 L 12 12 C 14 10 17 9 21 8.5"}};
// distortion: a sine with clipped peaks
constexpr IconPart kDistortion[] = {
    {K::Path, "M 2 12 C 2.8 8.2 4.2 6.5 6 6.5 H 9.5 C 10.8 6.5 11.3 9.5 12 12 C 12.7 14.5 13.2 17.5 14.5 17.5 H 18 "
              "C 19.8 17.5 21.2 15.8 22 12"}};
// ducker: a level that dips at the side-chain trigger and recovers
constexpr IconPart kDucker[] = {{K::Path, "M 7 3 V 21", {}, 0, 0, 0, 0, 0.4f},
                                {K::Path, "M 2 6 H 7 L 8 18 C 11 17 15 9 22 6"}};
// ext: any effect without its own drawing (P3 extensions, test effects)
constexpr IconPart kExt[] = {{K::Path, "M 4 4 H 20 V 20 H 4 Z"}, {K::Path, "M 12 8 V 16 M 8 12 H 16"}};

constexpr IconDef kIcons[] = {
    {"none", kNone},     {"chorus", kChorus}, {"flanger", kFlanger},       {"phaser", kPhaser},
    {"eq2", kEq2},       {"filter", kFilter}, {"delay", kDelay},           {"delayBus", kDelay},
    {"reverb", kReverb}, {"lofi", kLofi},     {"distortion", kDistortion}, {"compressor", kCompressor},
    {"ducker", kDucker}, {"ext", kExt}};

// --- flattening into primitives
// ---------------------------------------------------------------------------------------
struct Prim
{
    bool stroke;
    RectF r; // relative to the icon origin
    float radius, width, alpha;
};

struct Atlas
{
    static constexpr float sizes[] = {24, 40, 48};
    std::vector<Prim> prims;
    // [icon][size] → [begin, end)
    std::vector<std::array<std::pair<std::uint32_t, std::uint32_t>, 3>> ranges;
};

float strokeWidth(float size) noexcept
{
    return std::max(2.0f, 1.4f * size / 24.0f); // 2 lpx at 24, 2.8 lpx at 48
}

struct Flattener
{
    std::vector<Prim>& out;
    float k, w, alpha;

    void dot(float x, float y) { out.push_back({false, {x - w / 2, y - w / 2, w, w}, w / 2, 0, alpha}); }
    void line(float x1, float y1, float x2, float y2)
    {
        x1 *= k, y1 *= k, x2 *= k, y2 *= k;
        const float dx = x2 - x1, dy = y2 - y1, h = w / 2;
        if (std::abs(dx) < 1e-3f || std::abs(dy) < 1e-3f)
        {
            out.push_back(
                {false, {std::min(x1, x2) - h, std::min(y1, y2) - h, std::abs(dx) + w, std::abs(dy) + w}, h, 0, alpha});
            return;
        }
        const float pitch = std::max(0.75f, w * 0.35f);
        const int n = std::max(1, static_cast<int>(std::ceil(std::sqrt(dx * dx + dy * dy) / pitch)));
        for (int i = 1; i <= n; ++i) // the first point is the previous segment's last
        {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            dot(x1 + dx * t, y1 + dy * t);
        }
    }
    void path(std::string_view d)
    {
        float cx = 0, cy = 0, sx = 0, sy = 0;
        char cmd = 'M';
        std::size_t i = 0;
        auto skip = [&]
        {
            while (i < d.size() && (d[i] == ' ' || d[i] == ','))
                ++i;
        };
        auto number = [&]
        {
            skip();
            char buf[24];
            std::size_t n = 0;
            while (i < d.size() && n < sizeof buf - 1 &&
                   (std::isdigit(static_cast<unsigned char>(d[i])) || d[i] == '.' || d[i] == '-'))
                buf[n++] = d[i++];
            buf[n] = 0;
            return std::strtof(buf, nullptr);
        };
        while (true)
        {
            skip();
            if (i >= d.size())
                break;
            if (std::isalpha(static_cast<unsigned char>(d[i])))
                cmd = d[i++];
            switch (cmd)
            {
            case 'M':
                cx = sx = number();
                cy = sy = number();
                dot(cx * k, cy * k); // round cap at the start
                cmd = 'L';
                break;
            case 'L':
            {
                const float x = number(), y = number();
                line(cx, cy, x, y);
                cx = x, cy = y;
                break;
            }
            case 'H':
            {
                const float x = number();
                line(cx, cy, x, cy);
                cx = x;
                break;
            }
            case 'V':
            {
                const float y = number();
                line(cx, cy, cx, y);
                cy = y;
                break;
            }
            case 'C':
            {
                const float x1 = number(), y1 = number(), x2 = number(), y2 = number(), x = number(), y = number();
                constexpr int kSeg = 12;
                float px = cx, py = cy;
                for (int s = 1; s <= kSeg; ++s)
                {
                    const float t = static_cast<float>(s) / kSeg, u = 1 - t;
                    const float qx = u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
                    const float qy = u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
                    line(px, py, qx, qy);
                    px = qx, py = qy;
                }
                cx = x, cy = y;
                break;
            }
            case 'Z':
            case 'z':
                line(cx, cy, sx, sy);
                cx = sx, cy = sy;
                break;
            default:
                return; // unsupported command: stop (the SVG test catches it)
            }
        }
    }
    void part(const IconPart& p)
    {
        alpha = p.opacity;
        switch (p.kind)
        {
        case K::Path:
            path(p.d);
            break;
        case K::Circle:
            out.push_back({true, {(p.cx - p.r) * k, (p.cy - p.r) * k, 2 * p.r * k, 2 * p.r * k}, p.r * k, w, alpha});
            break;
        case K::Disc:
            out.push_back({false, {(p.cx - p.r) * k, (p.cy - p.r) * k, 2 * p.r * k, 2 * p.r * k}, p.r * k, 0, alpha});
            break;
        case K::Arc:
        {
            constexpr float rad = 3.14159265f / 180.0f;
            const float len = std::abs(p.a1 - p.a0) * rad * p.r * k;
            const int n = std::max(2, static_cast<int>(std::ceil(len / std::max(0.75f, w * 0.35f))));
            for (int s = 0; s <= n; ++s)
            {
                const float a = (p.a0 + (p.a1 - p.a0) * static_cast<float>(s) / static_cast<float>(n)) * rad;
                dot((p.cx + p.r * std::cos(a)) * k, (p.cy + p.r * std::sin(a)) * k);
            }
            break;
        }
        }
    }
};

Atlas& atlas()
{
    static Atlas a = []
    {
        Atlas at;
        at.prims.reserve(8192);
        for (const auto& icon : kIcons)
        {
            std::array<std::pair<std::uint32_t, std::uint32_t>, 3> r{};
            for (std::size_t s = 0; s < 3; ++s)
            {
                const float size = Atlas::sizes[s];
                const auto begin = static_cast<std::uint32_t>(at.prims.size());
                Flattener f{at.prims, size / 24.0f, strokeWidth(size), 1};
                for (const auto& p : icon.parts)
                    f.part(p);
                r[s] = {begin, static_cast<std::uint32_t>(at.prims.size())};
            }
            at.ranges.push_back(r);
        }
        return at;
    }();
    return a;
}

std::size_t indexOf(std::string_view id) noexcept
{
    for (std::size_t i = 0; i < std::size(kIcons); ++i)
        if (kIcons[i].id == id)
            return i;
    return std::size(kIcons) - 1; // ext
}

std::size_t sizeIndex(float size) noexcept
{
    return size <= 32 ? 0 : (size <= 44 ? 1 : 2);
}

void appendNumber(std::string& s, float v)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    s += buf;
}
} // namespace

std::span<const IconDef> effectIcons() noexcept
{
    return kIcons;
}

const IconDef* findEffectIcon(std::string_view id) noexcept
{
    for (const auto& i : kIcons)
        if (i.id == id)
            return &i;
    return nullptr;
}

std::string_view iconIdFor(std::string_view effectId) noexcept
{
    if (fx::isNone(effectId))
        return "none";
    if (const auto* spec = fx::environment().uiRegistry().find(effectId); spec != nullptr && ! spec->icon.empty())
        return spec->icon;
    return findEffectIcon(effectId) != nullptr ? effectId : std::string_view("ext");
}

std::string effectIconSvg(const IconDef& icon)
{
    std::string s = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"48\" height=\"48\" viewBox=\"0 0 24 24\" "
                    "fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\" "
                    "stroke-linejoin=\"round\">\n";
    s += "  <title>";
    s += icon.id;
    s += "</title>\n";
    for (const auto& p : icon.parts)
    {
        std::string opacity;
        if (p.opacity < 1)
        {
            opacity = " opacity=\"";
            appendNumber(opacity, p.opacity);
            opacity += "\"";
        }
        switch (p.kind)
        {
        case K::Path:
            s += "  <path d=\"";
            s += p.d;
            s += "\"" + opacity + "/>\n";
            break;
        case K::Circle:
        case K::Disc:
            s += "  <circle cx=\"";
            appendNumber(s, p.cx);
            s += "\" cy=\"";
            appendNumber(s, p.cy);
            s += "\" r=\"";
            appendNumber(s, p.r);
            s += p.kind == K::Disc ? "\" fill=\"currentColor\" stroke=\"none\"" : "\"";
            s += opacity + "/>\n";
            break;
        case K::Arc:
        {
            constexpr double rad = 3.14159265358979 / 180.0;
            const auto cx = static_cast<double>(p.cx), cy = static_cast<double>(p.cy), r = static_cast<double>(p.r);
            const double a0 = static_cast<double>(p.a0) * rad, a1 = static_cast<double>(p.a1) * rad;
            char buf[160];
            std::snprintf(buf, sizeof buf, "  <path d=\"M %.2f %.2f A %g %g 0 0 1 %.2f %.2f\"%s/>\n",
                          cx + r * std::cos(a0), cy + r * std::sin(a0), r, r, cx + r * std::cos(a1),
                          cy + r * std::sin(a1), opacity.c_str());
            s += buf;
            break;
        }
        }
    }
    s += "</svg>\n";
    return s;
}

void EffectIcon::warm()
{
    (void)atlas();
}

std::size_t EffectIcon::primitives(std::string_view id, float size)
{
    const auto& r = atlas().ranges[indexOf(id)][sizeIndex(size)];
    return r.second - r.first;
}

void EffectIcon::draw(DrawList& dl, std::string_view id, float x, float y, float size, std::uint32_t ink)
{
    const auto& a = atlas();
    const auto si = sizeIndex(size);
    const auto [begin, end] = a.ranges[indexOf(id)][si];
    const float scale = size / Atlas::sizes[si];
    const bool exact = std::abs(scale - 1) < 1e-4f;
    for (auto i = begin; i < end; ++i)
    {
        const auto& p = a.prims[i];
        const auto c = p.alpha < 1 ? theme::withOpacity(ink, p.alpha) : ink;
        const RectF r = exact ? RectF{x + p.r.x, y + p.r.y, p.r.w, p.r.h}
                              : RectF{x + p.r.x * scale, y + p.r.y * scale, p.r.w * scale, p.r.h * scale};
        if (p.stroke)
            dl.stroke(r, c, p.width * (exact ? 1 : scale), p.radius * (exact ? 1 : scale));
        else
            dl.fill(r, c, p.radius * (exact ? 1 : scale));
    }
}

void EffectIcon::build(DrawList& dl, const UiContext& ctx) const
{
    const int scene = state_ != nullptr ? state_->scene : 1, track = state_ != nullptr ? state_->selectedTrack : 1;
    const auto id = fx::environment().slotAccess().effect(slot_, scene, track);
    const float size = bounds_.h >= 48 ? 48 : 24;
    draw(dl, iconIdFor(id), bounds_.x + (bounds_.w - size) / 2, bounds_.y + (bounds_.h - size) / 2, size,
         ctx.palette.text2);
}

namespace
{
std::unique_ptr<Widget> makeEffectIcon(const ScreenSpec&, const WidgetArgs& args)
{
    const auto slot = args.binds == "fx2"      ? fx::Slot::Fx2
                      : args.binds == "send"   ? fx::Slot::Send
                      : args.binds == "master" ? fx::Slot::Master
                                               : fx::Slot::Fx1;
    return std::make_unique<EffectIcon>(args.bounds, args.state, slot);
}
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("EffectIcon", &ui::makeEffectIcon)
