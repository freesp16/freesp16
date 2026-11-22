// ANALOG FILTER — the PROCESSOR sub-page of the ANALOG FILTER SETTING popup (07-ui-design/screens/mixer.md "Popups" row
// 4 PROCESSOR › MODEL / EXTERNAL; AFS-004, AFS-005). Screen id `mixer.afs`, parent MIXER, template Full, context bar
// menu. The popup rows themselves are the MIXER's (MixerScreen.cpp, UID-556/557); the popup's PROCESSOR row opens this
// page.
//
// Body, top to bottom (canvas lpx; layout on the 16-lpx grid, text and colours from the theme tokens):
//   PROCESSOR   MODEL | EXTERNAL     two buttons, the current one Primary (afs.processor)
//   SLOPE       24 dB | 12 dB        the model's 4-pole output or its 2-pole tap (afs.slope)
//   LOOP        latency readout (afs.latency, the app's per-device preference: "-- " not measured, FAILED, or
//               "<n> smp · <ms> ms") and MEASURE, which posts afs.ping (the engine sends a −20 dBFS click on the
//               AFS SEND pair and correlates the AFS RETURN pair; a device without those ports runs the model)
//   hint        one line about AFS SEND / AFS RETURN (OUTPUT CHANNEL MAP, MIX-032)
// Knob strip (tab ANALOG FILTER): DRIVE · LP CUTOFF · RESONANCE · HP CUTOFF | ENV AMOUNT · LFO AMOUNT.
#include "ui/descriptors/ScreenSpec.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"

#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
constexpr RectF kModel{240, 184, 240, 72}, kExternal{496, 184, 240, 72}, kSteep{240, 288, 240, 72},
    kGentle{496, 288, 240, 72}, kMeasure{752, 392, 240, 72};
constexpr float kCaptionX = 48, kLatencyX = 240;

class AfsProcessorPage final : public Widget
{
public:
    explicit AfsProcessorPage(const WidgetArgs& a) noexcept : shell_(a.shell), bounds_(a.bounds)
    {
        if (a.params != nullptr)
        {
            processor_ = a.params->find("afs.processor");
            slope_ = a.params->find("afs.slope");
            ping_ = a.params->find("afs.ping");
            latency_ = a.params->find("afs.latency");
        }
    }
    RectF bounds() const noexcept override { return bounds_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        const bool external = value(processor_) >= 0.5f;
        const bool gentle = value(slope_) >= 0.5f;
        dl.text(kCaptionX, kModel.y + kModel.h / 2, Align::Left, TextStyleId::Label, p.text2, "PROCESSOR");
        Button::draw(dl, ctx, kModel, {"MODEL", external ? ButtonStyle::Plain : ButtonStyle::Primary});
        Button::draw(dl, ctx, kExternal, {"EXTERNAL", external ? ButtonStyle::Primary : ButtonStyle::Plain});
        dl.text(kCaptionX, kSteep.y + kSteep.h / 2, Align::Left, TextStyleId::Label, p.text2, "SLOPE");
        Button::draw(dl, ctx, kSteep, {"24 dB", gentle ? ButtonStyle::Plain : ButtonStyle::Primary, external});
        Button::draw(dl, ctx, kGentle, {"12 dB", gentle ? ButtonStyle::Primary : ButtonStyle::Plain, external});

        dl.text(kCaptionX, kMeasure.y + kMeasure.h / 2, Align::Left, TextStyleId::Label, p.text2, "LOOP");
        formatLatency();
        dl.text(kLatencyX, kMeasure.y + kMeasure.h / 2, Align::Left, TextStyleId::Value, external ? p.text1 : p.text3,
                std::string_view(text_, static_cast<std::size_t>(textLen_)));
        Button::draw(dl, ctx, kMeasure, {"MEASURE", ButtonStyle::Plain, ! external});
        dl.text(kCaptionX, 520, Align::Left, TextStyleId::Body, p.text3,
                "EXTERNAL sends on AFS SEND and returns from AFS RETURN; without them the model runs.");
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || ! touch::isTouch(e) || e.value != touch::kDown)
            return false;
        const float x = touch::x(e), y = touch::y(e);
        if (contains(kModel, x, y) || contains(kExternal, x, y))
            return set(processor_, contains(kExternal, x, y) ? 1.0f : 0.0f);
        const bool external = value(processor_) >= 0.5f;
        if (! external && (contains(kSteep, x, y) || contains(kGentle, x, y)))
            return set(slope_, contains(kGentle, x, y) ? 1.0f : 0.0f);
        if (external && contains(kMeasure, x, y) && ping_ != nullptr) // any new value is a measurement request
        {
            const float next = value(ping_) + 1.0f;
            return set(ping_, next > ping_->maxValue ? 0.0f : next);
        }
        return false;
    }

    bool dirty(double) const override
    {
        const float v = value(latency_);
        const bool d = v != shown_;
        shown_ = v;
        return d;
    }

private:
    float value(const core::ParamSpec* s) const
    {
        return s != nullptr && shell_ != nullptr ? shell_->values().get(*s, 0) : 0.0f;
    }
    bool set(const core::ParamSpec* s, float v)
    {
        if (s == nullptr)
            return false;
        shell_->values().set(*s, 0, v);
        return true;
    }
    void formatLatency() const
    {
        const long n = std::lround(value(latency_));
        if (n < 0)
            textLen_ = std::snprintf(text_, sizeof text_, "FAILED");
        else if (n == 0)
            textLen_ = std::snprintf(text_, sizeof text_, "--");
        else
            textLen_ = std::snprintf(text_, sizeof text_, "%ld smp  %.1f ms", n, static_cast<double>(n) * 1000.0 / 44100.0);
        if (textLen_ < 0)
            textLen_ = 0;
    }

    ShellApi* shell_ = nullptr;
    RectF bounds_ {};
    const core::ParamSpec* processor_ = nullptr;
    const core::ParamSpec* slope_ = nullptr;
    const core::ParamSpec* ping_ = nullptr;
    const core::ParamSpec* latency_ = nullptr;
    mutable char text_[48] {};
    mutable int textLen_ = 0;
    mutable float shown_ = -2.0f;
};

std::unique_ptr<Widget> makeAfsProcessorPage(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<AfsProcessorPage>(a);
}

constexpr StripSpec kAfsStrip {"ANALOG FILTER",
                               {"afs.drive", "afs.lpCutoff", "afs.resonance", "afs.hpCutoff", "afs.envAmount",
                                "afs.lfoAmount"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("AfsProcessorPage", &ui::makeAfsProcessorPage)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "mixer.afs",
                                    .title = "ANALOG FILTER",
                                    .parent = "mixer",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "AfsProcessorPage",
                                    .strip = ui::kAfsStrip}))
