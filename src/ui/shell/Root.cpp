// The UI root the composition root drives: the Shell (router, frame widgets,
// popups) with HOME as its root screen. Pads select and flash a track (the hit itself already reached the engine from
// the control source's reader thread, UI-118 — the UI never re-triggers); knob n edits column n of the bound strip and
// the change is posted to the engine (LocalParamValues → EngineBridge; track 1 carries the test tone); the selector
// moves the selection; BACK / HOME / SHIFT / steps / touches go through the shell's interaction model.
#include "ui/shell/Root.h"

#include "core/params/ParamRegistry.h"
#include "ui/input/transport/TransportController.h"
#include "ui/shell/Shell.h"

#include <cstdio>
#include <optional>

namespace ui
{
namespace
{
class ShellRoot final : public Root
{
public:
    explicit ShellRoot(const RootContext& ctx)
        : shell_(ShellConfig{ctx.params, ctx.measure, ctx.bridge, ctx.log, ctx.values}), bridge_(ctx.bridge),
          log_(ctx.log), beforeFrame_(ctx.beforeFrame)
    {
        // PLAY / STOP / REC / ext.tap / ext.nudge → the engine's transport; playhead → UiState.
        if (bridge_ != nullptr)
            transport_.emplace(*bridge_, &shell_.values(), ctx.params);
        // Without a model (beforeFrame unset), track 1 is the engine's 440 Hz test tone.
        auto& t1 = shell_.state().tracks[0];
        t1.name = "Test tone 440 Hz";
        t1.hasContent = true;
    }

    // "home": the root screen (the contract test pins it; Root.h had foreseen "shell").
    std::string_view id() const noexcept override { return "home"; }
    void handle(const io::ControlEvent& e, double nowMs) override
    {
        auto ev = e;
        ev.timeMs = nowMs;
        // SHIFT + ext.tap opens BPM/QUANTIZE (our choice): the shell takes it, the transport does not tap.
        const bool shiftTap = ev.control == io::Control::ExtTap && shell_.state().shift;
        if (transport_ && ! shiftTap && transport_->handle(ev) && ev.kind == io::ControlEvent::Kind::Press && log_)
        {
            const auto& p = bridge_->playhead(); // where the engine was when the key was pressed
            static constexpr const char* kStates[] = {"stopped", "playing", "paused", "pre-count"};
            char line[160];
            std::snprintf(line, sizeof line, "TRANSPORT %.*s -> %s ppq=%.3f bpm=%.1f playing=%d rec=%d",
                          static_cast<int>(io::nameOf(ev.control).size()), io::nameOf(ev.control).data(),
                          kStates[static_cast<int>(transport_->state()) & 3], p.ppq, p.bpm, p.playing ? 1 : 0,
                          transport_->recArmed() ? 1 : 0);
            log_(line);
        }
        shell_.dispatch(ev);
    }
    void frame(DrawList& dl, double nowMs) override
    {
        if (beforeFrame_) // the document model → UiState (app/Document)
            beforeFrame_(shell_.state(), nowMs);
        if (transport_) // after the model: the engine's playhead is the authority for the transport fields
        {
            transport_->sync(bridge_->playhead());
            transport_->publish(shell_.state());
        }
        shell_.frame(dl, nowMs);
    }
    const io::LampFrame* lamps() const noexcept override { return &shell_.lamps(); }
    ShellApi* shell() noexcept override { return &shell_; }

private:
    Shell shell_;
    engine::EngineBridge* bridge_;
    std::function<void(const std::string&)> log_;
    std::function<void(UiState&, double)> beforeFrame_;
    std::optional<input::transport::TransportController> transport_;
};
} // namespace

std::unique_ptr<Root> createRoot(const RootContext& ctx)
{
    return std::make_unique<ShellRoot>(ctx);
}
} // namespace ui
