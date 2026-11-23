// The Shell: the frame every screen lives in. It owns the Router, the frame
// widgets (StatusRow, ContextBar, KnobStrip), the current screen's rail and body widget, focus (UID-150, UI-009), the
// knob-strip binding from the StripSpec (UID-120/160…166: column n = encoder n, SHIFT fine steps, step-hold locks),
// the popup stack with its scrim and strip rebinding/dimming (UID-125, UID-900…916), toasts above the scrim, and the
// motion rules (UID-065: every animation is a function of time evaluated in build; input jumps it to its end).
//
// frame() appends the whole canvas to a DrawList (the renderer's damage tracking asks dirty()); dispatch() applies one
// logical control event immediately. Message thread only. After the first frames, frame() and dispatch() of
// encoder/pad/selector events do not allocate (tests/unit/ui/test_shell_alloc.cpp); navigation and popups allocate
// when they open.
//
// the shell owns the engine feeds (EngineFeeds, polled first in every frame; bodies read them through
// ShellApi::feeds() / WidgetArgs::feeds), routes a touch's drag and release to whoever took its down (the strip column,
// or the body — which may hand the drag to a strip column with ShellApi::dragColumn, UID-163), lets a body take part
// in its rail (RailOwner: screen actions, holes, custom-drawn items), presents custom popups (showPopup, popup
// factories by id), keeps the BYPASS mode (UI-221) and computes the lamp frame (LampModel, UI-150).
#pragma once

#include "core/params/ParamRegistry.h"
#include "core/util/CpuUsage.h"
#include "engine/EngineApi.h"
#include "ui/shell/FeatureFlags.h"
#include "ui/shell/Feeds.h"
#include "ui/shell/LampModel.h"
#include "ui/shell/LeaveGuard.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/Router.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/ContextBar.h"
#include "ui/widgets/KnobStrip.h"
#include "ui/widgets/RailItem.h"
#include "ui/widgets/StatusRow.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui
{
class Toast;
class Progress;

struct ShellConfig
{
    const core::ParamRegistry& params;
    TextMeasure measure;
    engine::EngineBridge* bridge =
        nullptr; // feeds and events in (EngineFeeds); parameter changes out (LocalParamValues)
    std::function<void(const std::string&)> log{}; // one line per navigation / input (emulator traces)
    ParamAccess* values = nullptr;                 // null: the shell keeps LocalParamValues
    ScreenRegistry* screens = nullptr;             // null: ScreenRegistry::builtin()
    WidgetFactory factory{};                       // empty: WidgetRegistry::builtin().dispatcher()
    FeatureFlags flags{};
    bool installFallbacks = true; // register interim screens the registry lacks (Fallbacks.h)
};

class Shell final : public ShellApi, public PopupPresenter
{
public:
    explicit Shell(ShellConfig);
    ~Shell() override;

    void frame(DrawList&, double nowMs);
    void dispatch(const io::ControlEvent&);
    // A physical keyboard (host preview): text goes to a text popup (UI-122); returns false when nothing consumed it.
    bool typeText(std::string_view utf8);
    bool textKey(TextKey);
    bool dirty(double nowMs) const; // something animates or changed since the last frame

    Router& router() noexcept { return router_; }
    const Router& router() const noexcept { return router_; }
    FocusState& focus() noexcept { return focus_; }
    const KnobStrip& strip() const noexcept { return strip_; }
    const ContextBar& contextBar() const noexcept { return contextBar_; }
    Widget* body() noexcept { return body_.get(); }
    int popupDepth() const noexcept; // modal popups open
    PopupWidget* topPopup() noexcept;
    const ParamColumnModel& column(int n0) const noexcept
    {
        return strip_.model().columns[static_cast<std::size_t>(n0)];
    }
    const core::ParamSpec* boundColumn(int n0) const noexcept { return bound_[static_cast<std::size_t>(n0)]; }
    std::string_view stripTab() const noexcept
    {
        if (! popupStrip_ && stripOwner_ != nullptr)
            if (const auto custom = stripOwner_->stripTab(); ! custom.empty())
                return custom;
        return tab_.view();
    }
    int railFocus() const noexcept { return focus_.focused; }

    // ShellApi
    bool open(std::string_view id) override;
    void back() override;
    void home() override;
    void selectTrack(int track1Based) override;
    void bindStrip(const StripSpec*) override;
    void showList(ListRequest) override;
    void showSetting(SettingRequest) override;
    void showGrid(GridRequest) override;
    void showMessage(MessageRequest) override;
    void showDialog(DialogRequest) override;
    Progress* showProgress(ProgressRequest) override;
    Toast* showToast(ToastRequest) override;
    // Persistent caution chips (UI-299, UID-909): one per cause id, newest last; `text` must outlive the caution.
    void setCaution(std::string_view id, std::string_view text);
    void clearCaution(std::string_view id);
    UiState& state() override { return state_; }
    ParamAccess& values() override { return *values_; }
    const core::ParamRegistry& params() const override { return params_; }
    const EngineFeeds& feeds() const override { return feeds_; }
    PopupWidget* showPopup(std::unique_ptr<PopupWidget>) override;
    void dragColumn(int column0) override;
    const ScreenSpec& current() const override { return router_.current(); }

    // The feeds the shell polls (tests inject snapshots and events here), this frame's engine events, and the lamp
    // frame of the last frame() (UI-150; the composition root resolves it with io::apply into Platform::lamps()).
    EngineFeeds& feeds() noexcept { return feeds_; }
    std::span<const engine::EngineEvent> engineEvents() const noexcept { return feeds_.events(); }
    // Computed on demand from the last frame's state (outside the frame build), once per frame.
    const io::LampFrame& lamps() const noexcept;
    int touchDragColumn() const noexcept { return dragColumn_; } // the strip column a running drag turns, -1 = none
    // UID-912: the ValueEntry over strip column n0 (a double-tap on the column opens it); false = nothing editable.
    bool openValueEntry(int column0);
    int closingPopups() const noexcept { return static_cast<int>(closing_.size()); } // close fades still running

    // PopupPresenter (Router::showPopup)
    bool present(const PopupSpec&) override;
    bool popupOpen() const override;
    void dismissTop() override;
    void dismissAll() override;

private:
    void log(const char* fmt, ...) const;
    void enterScreen(double nowMs);
    PopupWidget* pushPopup(std::unique_ptr<PopupWidget>);
    void resolveStrip();
    void updateFrameModels(double nowMs);
    void updateStripModel(double nowMs);
    void pollFeeds(double nowMs);
    void turnEncoder(int n0, int detents, double nowMs);
    void activateRail(int index, float x = -1, float y = -1);
    bool railFocusable(int index) const noexcept;
    RectF railRect(int index) const noexcept;
    static int padColourOf(const void* self, int track1Based);
    void moveRailFocus(int detents);
    bool jumpKey(io::Control);  // ext.mixer / ext.browse / ext.utility / ext.project (UI-102, UI-403)
    bool toggleRailFocus();     // SHIFT + sel.push: body <-> rail (our choice)
    void contextAction(ContextBar::Item);
    void handleTouch(const io::ControlEvent&, UiContext&);
    void interruptAnimations() noexcept;
    std::uint16_t scopeOf(const core::ParamSpec&) const noexcept;
    void eraseClosedPopups();
    UiContext context(const theme::tk::Palette&, double nowMs);

    const core::ParamRegistry& params_;
    TextMeasure measure_;
    engine::EngineBridge* bridge_;
    std::function<void(const std::string&)> log_;
    std::unique_ptr<ParamAccess> ownValues_;
    ParamAccess* values_;
    WidgetFactory factory_;
    FeatureFlags flags_;
    Router router_;
    UiState state_;
    FocusState focus_;

    StatusRow statusRow_;
    ContextBar contextBar_;
    KnobStrip strip_;
    std::vector<RailItem> rail_;
    std::unique_ptr<Widget> body_, meter_;
    std::uint32_t seenChanges_ = 0;
    double screenSinceMs_ = -1e9;
    int screenDirection_ = 0;
    DrawList slideList_; // the body during a screen push/pop slide (reserved, reused)

    const StripSpec* stripOverride_ = nullptr;
    std::array<const core::ParamSpec*, 6> bound_{};
    std::array<double, 6> touchedMs_{};
    std::uint8_t groupMask_ = 0;
    FixedText<24> tab_, tabShown_;
    FixedText<4> sceneText_, patternText_;
    double pressedAtMs_ = -1e9;
    const void* boundSource_ = nullptr;

    std::vector<std::unique_ptr<PopupWidget>> popups_;  // bottom → top; toasts kept apart
    std::vector<std::unique_ptr<PopupWidget>> closing_; // closed, still fading out (motion.base, UID-065)
    std::unique_ptr<Toast> toast_;
    struct Caution
    {
        std::string_view id, text;
    };
    std::array<Caution, 8> cautions_{};
    std::size_t cautionCount_ = 0;
    FixedText<8> moreText_;
    void showCautions();
    int dragColumn_ = -1;
    float dragY_ = 0;
    enum class TouchOwner : std::uint8_t
    {
        None,
        Frame, // status row, context bar, rail: acts on the down
        Strip,
        Body
    };
    TouchOwner touchOwner_ = TouchOwner::None;
    float touchY_ = 0;
    int touchDrags_ = 0; // drag events of the running touch (trace line on release)
    int lastTapColumn_ = -1;
    double lastTapMs_ = -1e9;
    RailOwner* railOwner_ = nullptr;         // the body, when it takes part in its rail
    StripOwner* stripOwner_ = nullptr;       // the body, when it refines its knob-strip columns
    bool popupStrip_ = false;                // the strip is bound to a popup's bindings (not the screen's)
    const LampSource* lampSource_ = nullptr; // the body, when it refines the lamps
    LeaveGuard* leaveGuard_ = nullptr;       // the body, when leaving its screen must be confirmed
    EngineFeeds feeds_;
    mutable LampModel lamps_;
    mutable double lampsAtMs_ = -1e18;
    double frameMs_ = 0;
    const core::ParamSpec* padColourSpec_ = nullptr; // track.padColor, looked up once (lamp colours per frame)
    const core::ParamSpec* showCpuSpec_ = nullptr;   // ui.showCpu (UTILITY › CPU USAGE), looked up once
    core::CpuUsage cpu_;                             // sampled once a second while CPU USAGE is ON
    double cpuSampledMs_ = -1e18;
    FixedText<8> cpuText_;                           // "37%", the status row's readout
    double nowMs_ = 0;
    bool changedSinceFrame_ = true;
};

// The interim screens the shell registers when no package provides them yet (ui/shell/Fallbacks.cpp).
void installFallbackScreens(ScreenRegistry&);
} // namespace ui
