#include "io/controllers/VirtualPanel.h"

#include "io/midi/MidiSettings.h"

#include <algorithm>
#include <cmath>

namespace io::controllers
{
namespace
{
using C = Control;
using K = PanelItemKind;
constexpr float kMinHit = 44;
constexpr double kDoubleClickMs = 300;

PanelItem key(C c, float x, float y, float w, float h, std::string_view label, LampStyle lamp = LampStyle::None,
              std::string_view shift = {})
{
    return {K::Key, c, {x, y, w, h}, {}, label, shift, lamp};
}
PanelItem round(K k, C c, float cx, float cy, float d, std::string_view label)
{
    return {k, c, {cx - d / 2, cy - d / 2, d, d}, {}, label, {}, LampStyle::None};
}
PanelItem lampItem(C c, PanelRect r, std::string_view label = {})
{
    return {K::Lamp, c, r, {}, label, {}, LampStyle::Dot};
}

std::vector<PanelItem> layout()
{
    std::vector<PanelItem> v;
    v.reserve(128);
    // Drawer bar buttons (before the bar: the first hit wins).
    v.push_back({K::PanelButton, C::Count, {1048, 4, 64, 24}, {}, "KEYS", {}, LampStyle::None, PanelButton::Keys});
    v.push_back({K::PanelButton, C::Count, {1120, 4, 64, 24}, {}, "FLOAT", {}, LampStyle::None, PanelButton::Float});
    v.push_back({K::PanelButton, C::Count, {1192, 4, 64, 24}, {}, "HIDE \xE2\x96\xBE", {}, LampStyle::None,
                 PanelButton::Hide});
    // Encoders under the knob strip columns.
    static constexpr std::string_view encNames[] = {"1", "2", "3", "4", "5", "6"};
    for (int n = 1; n <= 6; ++n)
        v.push_back(round(K::Encoder, knob(n), 120.0f + 208.0f * static_cast<float>(n - 1), 72, 56, encNames[n - 1]));
    // Strip block.
    v.push_back({K::Strip, C::Strip, {16, 124, 44, 268}, {}, "", {}, LampStyle::None});
    for (int k = 1; k <= 8; ++k)
        v.push_back(lampItem(static_cast<C>(static_cast<int>(C::StripLed1) + k - 1),
                             {66, 128 + 33.5f * static_cast<float>(8 - k), 6, 26}));
    v.push_back(key(C::StripMode, 88, 124, 88, 40, "MODE", LampStyle::Bar));
    static constexpr std::string_view modeNames[] = {"PITCH", "REPEAT", "USER1", "USER2"};
    for (int i = 0; i < 4; ++i)
        v.push_back(lampItem(static_cast<C>(static_cast<int>(C::StripModePitch) + i),
                             {94, 182 + 22.0f * static_cast<float>(i), 8, 8}, modeNames[i]));
    v.push_back(key(C::StripHold, 88, 352, 88, 40, "HOLD", LampStyle::Bar));
    // Pads 4 × 4 in pad order (13–16 top … 1–4 bottom).
    static constexpr std::string_view padNames[] = {"1", "2", "3", "4", "5", "6", "7", "8",
                                                    "9", "10", "11", "12", "13", "14", "15", "16"};
    for (int n = 1; n <= 16; ++n)
    {
        const int row = (n - 1) / 4, col = (n - 1) % 4;
        v.push_back({K::Pad, pad(n), {192.0f + 100.0f * static_cast<float>(col), 292.0f - 56.0f * static_cast<float>(row), 92, 48},
                     {}, padNames[n - 1], {}, LampStyle::Fill});
    }
    // Function row.
    v.push_back(key(C::PadModeTrack, 192, 352, 92, 40, "TRACK", LampStyle::Bar));
    v.push_back(key(C::PadModeMute, 292, 352, 92, 40, "MUTE", LampStyle::Bar));
    v.push_back(key(C::PadModeSlice, 392, 352, 92, 40, "SLICE", LampStyle::Bar));
    v.push_back(key(C::PadModeScale, 492, 352, 92, 40, "SCALE", LampStyle::Bar));
    v.push_back(key(C::Shift, 608, 352, 72, 40, "SHIFT"));
    v.push_back(key(C::Rec, 688, 352, 72, 40, "\xE2\x97\x8F REC", LampStyle::Bar));    // ●
    v.push_back(key(C::Stop, 768, 352, 72, 40, "\xE2\x96\xA0 STOP", LampStyle::Bar));   // ■
    v.push_back(key(C::Play, 848, 352, 72, 40, "\xE2\x96\xB6 PLAY", LampStyle::Bar));   // ▶
    v.push_back(key(C::Scene, 936, 352, 88, 40, "SCENE", LampStyle::Bar));
    v.push_back(key(C::Pattern, 1032, 352, 88, 40, "PATTERN", LampStyle::Bar, "LENGTH"));
    // MAIN OUT and the analog filter.
    v.push_back(round(K::Knob, C::MainVolume, 640, 148, 40, "MAIN OUT"));
    v.push_back(round(K::Knob, C::AfsDrive, 728, 156, 40, "DRIVE"));
    v.push_back(lampItem(C::AfsOver, {779, 151, 10, 10}, "OVER"));
    v.push_back(round(K::Knob, C::AfsCutoff, 848, 156, 40, "LP CUTOFF"));
    v.push_back(round(K::Knob, C::AfsResonance, 936, 156, 40, "RESONANCE"));
    v.push_back(round(K::Knob, C::AfsHpCutoff, 1048, 156, 40, "HP CUTOFF"));
    v.push_back(key(C::AfsActive, 1136, 132, 80, 40, "ACTIVE", LampStyle::Bar));
    // SIXTEEN KEYS (UI-102).
    static constexpr C ext[3][5] = {{C::ExtCopy, C::ExtPaste, C::ExtClear, C::ExtUndo, C::ExtRedo},
                                    {C::ExtTap, C::ExtNudgeDown, C::ExtNudgeUp, C::ExtCue, C::ExtErase},
                                    {C::ExtAccent, C::ExtMixer, C::ExtBrowse, C::ExtUtility, C::ExtProject}};
    static constexpr std::string_view extNames[3][5] = {{"COPY", "PASTE", "CLEAR", "UNDO", "REDO"},
                                                        {"TAP", "NUDGE \xE2\x88\x92", "NUDGE +", "CUE", "ERASE"},
                                                        {"ACCENT", "MIXER", "BROWSE", "UTILITY", "PROJECT"}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 5; ++c)
            v.push_back(key(ext[r][c], 608.0f + 86.0f * static_cast<float>(c), 216.0f + 44.0f * static_cast<float>(r), 80,
                            40, extNames[r][c]));
    v.push_back(key(C::Back, 1048, 216, 96, 40, "\xE2\x86\xB6 BACK"));                    // ↶
    v.push_back(key(C::Home, 1048, 260, 96, 40, "\xE2\x8C\x82 HOME", LampStyle::None, "UTILITY")); // ⌂
    v.push_back(round(K::Rotary, C::Selector, 1204, 296, 104, "PUSH"));
    // Step keys and BAR keys.
    for (int n = 1; n <= 16; ++n)
        v.push_back({K::Step, step(n), {16.0f + 60.0f * static_cast<float>(n - 1), 424, 52, 44}, {}, padNames[n - 1], {},
                     LampStyle::Fill});
    static constexpr std::string_view barNames[] = {"BAR 1", "BAR 2", "BAR 3", "BAR 4"};
    for (int n = 1; n <= 4; ++n)
    {
        const float x = 1000.0f + 66.0f * static_cast<float>(n - 1);
        v.push_back(lampItem(barPositionLamp(n), {x, 412, 56, 4}));
        v.push_back(key(static_cast<C>(static_cast<int>(C::Bar1) + n - 1), x, 424, 56, 44, barNames[n - 1], LampStyle::Fill));
    }
    // The drawer bar itself last (its buttons above win).
    v.push_back({K::PanelButton, C::Count, {0, 0, 1280, 32}, {}, "CONTROL PANEL", {}, LampStyle::None, PanelButton::None});

    // Hit areas: grown to 44 × 44, kept inside the panel, never into a neighbour (UID-935).
    for (auto& it : v)
    {
        if (it.kind == K::Lamp)
            continue;
        PanelRect h = it.rect;
        if (it.button == PanelButton::None && it.kind == K::PanelButton)
        {
            it.hit = h;
            continue;
        }
        if (h.w < kMinHit)
        {
            h.x -= (kMinHit - h.w) / 2;
            h.w = kMinHit;
        }
        if (h.h < kMinHit)
        {
            h.y -= (kMinHit - h.h) / 2;
            h.h = kMinHit;
        }
        h.x = std::clamp(h.x, 0.0f, VirtualPanel::kWidth - h.w);
        h.y = std::clamp(h.y, 0.0f, VirtualPanel::kHeight - h.h);
        it.hit = h;
    }
    for (std::size_t a = 0; a < v.size(); ++a)
        for (std::size_t b = a + 1; b < v.size(); ++b)
        {
            auto& A = v[a];
            auto& B = v[b];
            if (A.kind == K::Lamp || B.kind == K::Lamp || (A.kind == K::PanelButton && A.button == PanelButton::None)
                || (B.kind == K::PanelButton && B.button == PanelButton::None))
                continue;
            auto& ha = A.hit;
            auto& hb = B.hit;
            const bool overlap = ha.x < hb.x + hb.w && hb.x < ha.x + ha.w && ha.y < hb.y + hb.h && hb.y < ha.y + ha.h;
            if (! overlap)
                continue;
            const auto& ra = A.rect;
            const auto& rb = B.rect;
            // Split at the middle of the gap between the drawn rects, along the axis they are apart on.
            auto split = [](float& lo0, float& lo1, float& hi0, float& hi1, float edgeLo, float edgeHi) {
                const float mid = (edgeLo + edgeHi) * 0.5f;
                lo1 = std::min(lo0 + lo1, mid) - lo0; // width of the lower one
                const float hiEnd = hi0 + hi1;
                hi0 = std::max(hi0, mid);
                hi1 = hiEnd - hi0;
            };
            if (ra.x + ra.w <= rb.x)
                split(ha.x, ha.w, hb.x, hb.w, ra.x + ra.w, rb.x);
            else if (rb.x + rb.w <= ra.x)
                split(hb.x, hb.w, ha.x, ha.w, rb.x + rb.w, ra.x);
            else if (ra.y + ra.h <= rb.y)
                split(ha.y, ha.h, hb.y, hb.h, ra.y + ra.h, rb.y);
            else if (rb.y + rb.h <= ra.y)
                split(hb.y, hb.h, ha.y, ha.h, rb.y + rb.h, ra.y);
        }
    return v;
}

bool holdable(K k) noexcept { return k == K::Key || k == K::Pad || k == K::Step; }
} // namespace

int knobDefault(Control c) noexcept
{
    return c == Control::MainVolume ? 819 : 512;
}

VirtualPanel::VirtualPanel() : items_(layout())
{
    pending_.reserve(64);
    for (const auto& it : items_)
        if (it.kind == K::Knob)
            knob_[slotOf(it.control)] = static_cast<std::int16_t>(knobDefault(it.control));
}

void VirtualPanel::poll(std::vector<ControlEvent>& out)
{
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void VirtualPanel::setLamp(Control c, std::uint32_t rgb, std::uint8_t level)
{
    const auto s = slotOf(c);
    if (s < lamps_.size())
        lamps_[s] = {rgb & 0xFFFFFFu, level};
}

void VirtualPanel::emit(ControlEvent::Kind k, Control c, int v, double t)
{
    pending_.push_back({k, c, static_cast<std::int16_t>(v), kSourceVirtual, t});
}

const PanelItem* VirtualPanel::itemAt(float x, float y) const noexcept
{
    for (const auto& it : items_)
        if (it.kind != K::Lamp && it.hit.contains(x, y))
            return &it;
    return nullptr;
}

const PanelItem* VirtualPanel::item(Control c) const noexcept
{
    for (const auto& it : items_)
        if (it.control == c && it.kind != K::PanelButton)
            return &it;
    return nullptr;
}

VirtualPanel::Lamp VirtualPanel::lamp(Control c) const noexcept
{
    const auto s = slotOf(c);
    return s < lamps_.size() ? lamps_[s] : Lamp {};
}

bool VirtualPanel::pressed(Control c) const noexcept
{
    const auto s = slotOf(c);
    return s < held_.size() && (held_[s] > 0 || latched_[s] || external_[s]);
}

bool VirtualPanel::latched(Control c) const noexcept
{
    const auto s = slotOf(c);
    return s < latched_.size() && latched_[s];
}

int VirtualPanel::knobValue(Control c) const noexcept { return knob_[slotOf(c)]; }
double VirtualPanel::lastTurnMs(Control c) const noexcept { return turned_[slotOf(c)]; }
int VirtualPanel::encoderAngle(Control c) const noexcept { return angle_[slotOf(c)]; }

void VirtualPanel::toggle() noexcept
{
    state_ = state_ == PanelState::Expanded ? PanelState::Hidden : PanelState::Expanded;
}

void VirtualPanel::turn(Control c, int detents, bool fine, double t)
{
    if (detents == 0)
        return;
    const bool wrap = fine && ! pressed(Control::Shift);
    if (wrap)
        emit(ControlEvent::Kind::Press, Control::Shift, 0, t);
    emit(ControlEvent::Kind::Turn, c, detents, t);
    if (wrap)
        emit(ControlEvent::Kind::Release, Control::Shift, 0, t);
    turned_[slotOf(c)] = t;
    angle_[slotOf(c)] += detents;
}

void VirtualPanel::press(Pointer& p, const PanelItem& it, Mods mods, double t)
{
    const auto s = slotOf(it.control);
    if (latched_[s]) // a click on a latched control lets it go
    {
        latched_[s] = false;
        if (held_[s] == 0)
            emit(ControlEvent::Kind::Release, it.control, 0, t);
        p.item = nullptr;
        return;
    }
    p.shiftWrap = mods.shift && it.control != Control::Shift && ! pressed(Control::Shift);
    if (p.shiftWrap)
        emit(ControlEvent::Kind::Press, Control::Shift, 0, t);
    int v = 0;
    if (it.kind == K::Pad)
    {
        const float f = std::clamp((p.y0 - it.rect.y) / it.rect.h, 0.0f, 1.0f);
        v = mods.option ? 127 : std::clamp(static_cast<int>(std::lround(127.0f - f * 107.0f)), 20, 127);
        p.velocity = v;
    }
    if (held_[s]++ == 0 && ! external_[s])
        emit(ControlEvent::Kind::Press, it.control, v, t);
    if (mods.secondary || mods.control) // right-click / Control-click: latch at once
    {
        latched_[s] = true;
        p.latchedNow = true;
    }
}

void VirtualPanel::release(Pointer& p, double t)
{
    if (p.item == nullptr)
        return;
    const auto& it = *p.item;
    const auto s = slotOf(it.control);
    if (held_[s] > 0)
        --held_[s];
    if (held_[s] == 0 && ! latched_[s] && ! external_[s])
        emit(ControlEvent::Kind::Release, it.control, 0, t);
    if (p.shiftWrap)
        emit(ControlEvent::Kind::Release, Control::Shift, 0, t);
}

bool VirtualPanel::pointerDown(int id, float x, float y, Mods mods, double t)
{
    if (id < 0 || id >= kPointers || state_ == PanelState::Hidden)
        return false;
    if (state_ == PanelState::Collapsed && y >= kBarHeight)
        return false;
    const PanelItem* it = itemAt(x, y);
    if (it == nullptr)
        return state_ == PanelState::Expanded; // a click on the panel's background is still the panel's
    auto& p = pointers_[static_cast<std::size_t>(id)];
    p = {};
    p.active = true;
    p.item = it;
    p.x0 = p.x = x;
    p.y0 = p.y = y;
    p.t0 = t;
    p.fine = mods.option;
    switch (it->kind)
    {
    case K::PanelButton:
        if (it->button == PanelButton::Keys)
            keycaps_ = ! keycaps_;
        else if (it->button == PanelButton::Hide)
            state_ = PanelState::Hidden;
        else if (it->button == PanelButton::None)
            state_ = state_ == PanelState::Expanded ? PanelState::Collapsed : PanelState::Expanded;
        p.item = nullptr; // FLOAT: unavailable on a single surface
        return true;
    case K::Key:
    case K::Pad:
    case K::Step: press(p, *it, mods, t); return true;
    case K::Rotary:
    {
        const float dx = x - it->rect.cx(), dy = y - it->rect.cy();
        if (dx * dx + dy * dy <= 26.0f * 26.0f) // the centre = sel.push
        {
            static const PanelItem push {K::Key, Control::SelectorPush, {}, {}, "PUSH"};
            p.item = &push;
            press(p, push, mods, t);
        }
        else
            p.acc = std::atan2(dy, dx);
        return true;
    }
    case K::Knob:
    {
        auto& last = lastClick_[slotOf(it->control)];
        if (t - last <= kDoubleClickMs)
        {
            knob_[slotOf(it->control)] = static_cast<std::int16_t>(knobDefault(it->control));
            emit(ControlEvent::Kind::Value, it->control, knobDefault(it->control), t);
            last = -1.0e9;
        }
        else
            last = t;
        p.velocity = knob_[slotOf(it->control)]; // the value the drag starts from
        return true;
    }
    case K::Strip:
    {
        const float f = std::clamp((y - it->rect.y) / it->rect.h, 0.0f, 1.0f);
        stripPos_ = std::clamp(static_cast<int>(std::lround((1.0f - f) * 127.0f)), 0, 127);
        emit(ControlEvent::Kind::Touch, Control::Strip, stripPos_, t);
        return true;
    }
    default: return true;
    }
}

bool VirtualPanel::pointerMove(int id, float x, float y, double t)
{
    if (id < 0 || id >= kPointers)
        return false;
    auto& p = pointers_[static_cast<std::size_t>(id)];
    if (! p.active)
        return false;
    const float dx = x - p.x, dy = y - p.y;
    if (std::fabs(x - p.x0) > 4 || std::fabs(y - p.y0) > 4)
        p.moved = true;
    p.x = x;
    p.y = y;
    if (p.item == nullptr)
        return true;
    const auto& it = *p.item;
    switch (it.kind)
    {
    case K::Pad:
    {
        const int pressure = std::clamp(static_cast<int>((p.y0 - y) * 2.0f), 0, 127);
        if (pressure != p.velocity && held_[slotOf(it.control)] > 0)
        {
            emit(ControlEvent::Kind::Pressure, it.control, pressure, t);
            p.velocity = pressure;
        }
        break;
    }
    case K::Encoder:
        p.acc += -dy; // up = +
        while (std::fabs(p.acc) >= 8.0f)
        {
            const int d = p.acc > 0 ? 1 : -1;
            p.acc -= static_cast<float>(d) * 8.0f;
            turn(it.control, d, p.fine, t);
        }
        break;
    case K::Rotary:
    {
        const float a = std::atan2(y - it.rect.cy(), x - it.rect.cx());
        float d = a - p.acc;
        while (d > 3.14159265f)
            d -= 6.2831853f;
        while (d < -3.14159265f)
            d += 6.2831853f;
        constexpr float step15 = 15.0f * 3.14159265f / 180.0f;
        if (std::fabs(d) >= step15)
        {
            const int n = static_cast<int>(d / step15);
            p.acc += static_cast<float>(n) * step15;
            turn(Control::Selector, n, p.fine, t);
        }
        break;
    }
    case K::Knob:
    {
        const int v = std::clamp(p.velocity + static_cast<int>(std::lround((p.y0 - y) * 1023.0f / 200.0f)), 0, 1023);
        auto& k = knob_[slotOf(it.control)];
        if (v != k)
        {
            k = static_cast<std::int16_t>(v);
            emit(ControlEvent::Kind::Value, it.control, v, t);
            turned_[slotOf(it.control)] = t;
        }
        break;
    }
    case K::Strip:
    {
        const float f = std::clamp((y - it.rect.y) / it.rect.h, 0.0f, 1.0f);
        const int pos = std::clamp(static_cast<int>(std::lround((1.0f - f) * 127.0f)), 0, 127);
        if (pos != stripPos_)
        {
            stripPos_ = pos;
            emit(ControlEvent::Kind::Value, Control::Strip, pos, t);
        }
        break;
    }
    default: break;
    }
    (void)dx;
    return true;
}

bool VirtualPanel::pointerUp(int id, float x, float y, double t)
{
    if (id < 0 || id >= kPointers)
        return false;
    auto& p = pointers_[static_cast<std::size_t>(id)];
    if (! p.active)
        return false;
    pointerMove(id, x, y, t);
    p.active = false;
    if (p.item == nullptr)
        return true;
    switch (p.item->kind)
    {
    case K::Key:
    case K::Pad:
    case K::Step: release(p, t); break;
    case K::Strip:
        emit(ControlEvent::Kind::Release, Control::Strip, stripPos_, t);
        stripPos_ = -1;
        break;
    default: break;
    }
    p.item = nullptr;
    return true;
}

bool VirtualPanel::wheel(float x, float y, int notches, Mods mods, double t)
{
    if (state_ != PanelState::Expanded || notches == 0)
        return false;
    const PanelItem* it = itemAt(x, y);
    if (it == nullptr)
        return true;
    if (it->kind == K::Encoder || it->kind == K::Rotary)
        turn(it->control, notches, mods.option, t);
    else if (it->kind == K::Knob)
    {
        auto& k = knob_[slotOf(it->control)];
        k = static_cast<std::int16_t>(std::clamp(k + notches * 8, 0, 1023));
        emit(ControlEvent::Kind::Value, it->control, k, t);
        turned_[slotOf(it->control)] = t;
    }
    return true;
}

void VirtualPanel::tick(double t)
{
    for (auto& p : pointers_)
        if (p.active && p.item != nullptr && ! p.latchedNow && ! p.moved && holdable(p.item->kind)
            && t - p.t0 >= kLatchMs)
        {
            latched_[slotOf(p.item->control)] = true; // a 500 ms hold latches (UID-931)
            p.latchedNow = true;
        }
}

void VirtualPanel::observe(const ControlEvent& e)
{
    if (e.source == kSourceVirtual)
        return;
    const auto s = slotOf(e.control);
    if (s >= external_.size())
        return;
    if (e.kind == ControlEvent::Kind::Press)
        external_[s] = true;
    else if (e.kind == ControlEvent::Kind::Release)
        external_[s] = false;
    else if (e.kind == ControlEvent::Kind::Turn)
    {
        turned_[s] = e.timeMs;
        angle_[s] += e.value;
    }
    else if (e.kind == ControlEvent::Kind::Value && controlInfo(e.control).kind == ControlKind::Absolute)
        knob_[s] = static_cast<std::int16_t>(std::clamp<int>(e.value, 0, 1023));
}

VirtualPanel& virtualPanel()
{
    static VirtualPanel* panel = new VirtualPanel(); // process lifetime, like the other registered sources' state
    return *panel;
}

namespace
{
class PanelSource final : public ControlSource, public LampSink
{
public:
    std::string_view id() const override { return "virtual-panel"; }
    bool start(std::string& info) override
    {
        const std::string s = midi::setting("virtual_panel", "hidden");
        auto& p = virtualPanel();
        p.setState(s == "expanded" ? PanelState::Expanded : s == "collapsed" ? PanelState::Collapsed : PanelState::Hidden);
        info = "1280x480 lpx, " + std::to_string(p.items().size()) + " items, " + s + " (Ctrl+\\ toggles)";
        return true;
    }
    void poll(std::vector<ControlEvent>& out) override { virtualPanel().poll(out); }
    void setLamp(Control c, std::uint32_t rgb, std::uint8_t level) override { virtualPanel().setLamp(c, rgb, level); }
    void flush() override {}
};
std::unique_ptr<ControlSource> makePanelSource(engine::TriggerSink*)
{
    return std::make_unique<PanelSource>();
}
} // namespace
} // namespace io::controllers

APP_REGISTER_CONTROL_SOURCE("virtual-panel", &io::controllers::makePanelSource)
