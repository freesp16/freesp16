// UI bridge to hosted CLAP plugins on TrackMode::Plugin tracks (mirrors ui/screens/effects/FxSlots.h):
//   - PluginAccess: catalogue discovery, per-(scene, track) selection, bypass, presets, 6-knob strip bank, and
//     optional custom body screen surface (org.sixteen.screen/1, 780 x 285 px ARGB8888 inside BodyTemplate::Full).
//   - LocalPluginAccess: lightweight in-memory default for standalone UI tests and previews.
//   - PluginEnvironment / ScopedPluginEnvironment: process-wide hook installed by app/ (composition root) or tests.
// Message thread only. Per-frame read methods (readKnobs, screenRender, bypass, hasLoaded) do not allocate.
#pragma once

#include "core/util/Result.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/ParamColumn.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
class ShellApi;
namespace fx
{
enum class Slot : std::uint8_t;
}
} // namespace ui

namespace ui::plugin
{
void showEffectPresets(ShellApi&, fx::Slot, int scene, int track);
void showEffectPicker(ShellApi&, fx::Slot, int scene, int track);

struct EffectPluginView
{
    std::string_view path, id, name;
    bool loaded = false;
};
inline constexpr int kScreenWidth = 780;
inline constexpr int kScreenHeight = 285;

struct PluginEntry
{
    std::string id{};     // CLAP plugin descriptor id (e.g. "com.hdslave.anvil")
    std::string name{};   // display name (e.g. "Anvil")
    std::string vendor{}; // vendor string (optional)
    std::string path{};   // path relative to <data>/Plugins/ (or absolute)
    bool hasCustomScreen = false;
    bool trackCompatible = true;
    bool effectCompatible = false;
};

struct PluginPresetEntry
{
    std::string key{};
    std::string name{};
    bool user = false;
};

struct PluginKnobView
{
    bool active = false;
    bool stepped = false;
    bool bipolar = false;
    std::uint32_t paramId = 0;
    float norm = 0.0f; // 0..1 normalized value for the KnobStrip bar
    FixedText<24> label{};
    FixedText<24> valueText{};
};

struct PluginKnobBank
{
    FixedText<24> tab{};
    std::array<PluginKnobView, 6> columns{};
};

void knobColumn(const PluginKnobBank&, int col, core::ParamSpec&, ParamColumnModel&);

struct PluginScreenFrame
{
    const std::uint32_t* argb = nullptr; // 0xAARRGGBB, kScreenWidth x kScreenHeight
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint32_t serial = 0; // monotonic counter; changes only when pixels changed
};

// All `scene` and `track` indices are 1-based (1..16), matching FxSlotAccess.
class PluginAccess
{
public:
    virtual ~PluginAccess() = default;

    virtual std::vector<PluginEntry> catalogue() const = 0;
    virtual void rescan() {}

    virtual bool hasLoaded(int scene, int track) const noexcept = 0;
    virtual std::string_view loadedName(int scene, int track) const noexcept = 0;
    virtual std::string_view loadedVendor(int scene, int track) const noexcept = 0;
    virtual std::string_view loadedId(int scene, int track) const noexcept = 0;
    virtual std::string_view loadedPath(int scene, int track) const noexcept = 0;

    virtual bool selectPlugin(int scene, int track, std::string_view path, std::string_view pluginId) = 0;
    virtual EffectPluginView effectPlugin(fx::Slot, int, int) const { return {}; }
    virtual bool selectEffectPlugin(fx::Slot, int, int, const PluginEntry&) { return false; }
    virtual void clearPlugin(int scene, int track) = 0;

    virtual bool bypass(int scene, int track) const noexcept = 0;
    virtual void setBypass(int scene, int track, bool on) = 0;

    virtual std::vector<PluginPresetEntry> presets(int scene, int track, std::optional<fx::Slot> slot = {}) const = 0;
    virtual std::string_view currentPreset(int scene, int track, std::optional<fx::Slot> slot = {}) const noexcept = 0;
    virtual bool loadPreset(int scene, int track, const PluginPresetEntry& preset,
                            std::optional<fx::Slot> slot = {}) = 0;

    virtual std::string presetNameError(const std::string& name) const { return name.empty() ? "Enter a name" : ""; }
    // true = saved, false = the name exists and needs explicit replacement.
    virtual core::Result<bool> savePreset(int, int, std::optional<fx::Slot>, const std::string&, bool)
    {
        return core::Error{"Preset storage is unavailable"};
    }

    // Active 6-knob bank (custom screen page bank OR generic 6-per-page CLAP parameter bank).
    virtual bool readKnobs(int scene, int track, PluginKnobBank& out,
                           std::optional<ui::fx::Slot> = {}) const noexcept = 0;
    virtual bool turnKnob(int scene, int track, int column0, int detents, bool fine,
                          std::optional<ui::fx::Slot> = {}) noexcept = 0;
    virtual bool pushKnob(int scene, int track, int column0) noexcept = 0;

    // Generic parameter paging when the plugin does not provide org.sixteen.screen/1.
    virtual int pageCount(int scene, int track) const noexcept = 0;
    virtual int currentPage(int scene, int track) const noexcept = 0;
    virtual void setPage(int scene, int track, int page) noexcept = 0;

    // Custom body screen (org.sixteen.screen/1) — 780 x 285 physical px inside BodyTemplate::Full.
    virtual bool hasCustomScreen(int scene, int track, std::optional<ui::fx::Slot> = {}) const noexcept = 0;
    virtual PluginScreenFrame screenRender(int scene, int track, std::uint32_t nowMs,
                                           std::optional<ui::fx::Slot> = {}) noexcept = 0;
    // phase: 0 = down, 1 = move, 2 = up; x in 0..779, y in 0..284.
    virtual bool screenTouch(int scene, int track, int phase, int x, int y,
                             std::optional<ui::fx::Slot> = {}) noexcept = 0;
    virtual bool screenSelectorTurn(int scene, int track, int delta, std::optional<ui::fx::Slot> = {}) noexcept = 0;
    virtual bool screenSelectorPush(int scene, int track, std::optional<ui::fx::Slot> = {}) noexcept = 0;

    virtual std::uint32_t changes() const noexcept = 0;
};

// Lightweight in-memory implementation for UI unit tests and standalone previews.
class LocalPluginAccess final : public PluginAccess
{
public:
    LocalPluginAccess();

    void setCatalogue(std::vector<PluginEntry> entries)
    {
        catalogue_ = std::move(entries);
        ++changes_;
    }

    std::vector<PluginEntry> catalogue() const override { return catalogue_; }
    bool hasLoaded(int scene, int track) const noexcept override;
    std::string_view loadedName(int scene, int track) const noexcept override;
    std::string_view loadedVendor(int scene, int track) const noexcept override;
    std::string_view loadedId(int scene, int track) const noexcept override;
    std::string_view loadedPath(int scene, int track) const noexcept override;

    bool selectPlugin(int scene, int track, std::string_view path, std::string_view pluginId) override;
    void clearPlugin(int scene, int track) override;

    bool bypass(int scene, int track) const noexcept override;
    void setBypass(int scene, int track, bool on) override;

    std::vector<PluginPresetEntry> presets(int scene, int track, std::optional<fx::Slot> slot = {}) const override;
    std::string_view currentPreset(int scene, int track, std::optional<fx::Slot> slot = {}) const noexcept override;
    bool loadPreset(int scene, int track, const PluginPresetEntry& preset, std::optional<fx::Slot> slot = {}) override;

    bool readKnobs(int scene, int track, PluginKnobBank& out, std::optional<ui::fx::Slot> = {}) const noexcept override;
    bool turnKnob(int scene, int track, int column0, int detents, bool fine,
                  std::optional<ui::fx::Slot> = {}) noexcept override;
    bool pushKnob(int scene, int track, int column0) noexcept override;

    int pageCount(int scene, int track) const noexcept override;
    int currentPage(int scene, int track) const noexcept override;
    void setPage(int scene, int track, int page) noexcept override;

    bool hasCustomScreen(int scene, int track, std::optional<ui::fx::Slot> = {}) const noexcept override
    {
        (void)scene, (void)track;
        return false;
    }
    PluginScreenFrame screenRender(int scene, int track, std::uint32_t nowMs,
                                   std::optional<ui::fx::Slot> = {}) noexcept override
    {
        (void)scene, (void)track, (void)nowMs;
        return {};
    }
    bool screenTouch(int scene, int track, int phase, int x, int y, std::optional<ui::fx::Slot> = {}) noexcept override
    {
        (void)scene, (void)track, (void)phase, (void)x, (void)y;
        return false;
    }
    bool screenSelectorTurn(int scene, int track, int delta, std::optional<ui::fx::Slot> = {}) noexcept override
    {
        (void)scene, (void)track, (void)delta;
        return false;
    }
    bool screenSelectorPush(int scene, int track, std::optional<ui::fx::Slot> = {}) noexcept override
    {
        (void)scene, (void)track;
        return false;
    }

    std::uint32_t changes() const noexcept override { return changes_; }

private:
    struct Slot
    {
        bool loaded = false;
        bool bypassed = false;
        FixedText<48> id{};
        FixedText<48> name{};
        FixedText<48> vendor{};
        FixedText<96> path{};
        FixedText<32> preset{};
        int page = 0;
        std::array<float, 6> params{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    };
    static std::size_t idx(int scene, int track) noexcept
    {
        return static_cast<std::size_t>((scene - 1) & 15) * 16 + static_cast<std::size_t>((track - 1) & 15);
    }

    std::vector<PluginEntry> catalogue_;
    std::array<Slot, 256> slots_{};
    std::uint32_t changes_ = 0;
};

struct PluginEnvironment
{
    PluginAccess* access = nullptr;
    PluginAccess& pluginAccess() const noexcept;
};

PluginEnvironment& environment() noexcept;

class ScopedPluginEnvironment
{
public:
    explicit ScopedPluginEnvironment(const PluginEnvironment& env) : saved_(environment()) { environment() = env; }
    ~ScopedPluginEnvironment() { environment() = saved_; }
    ScopedPluginEnvironment(const ScopedPluginEnvironment&) = delete;
    ScopedPluginEnvironment& operator=(const ScopedPluginEnvironment&) = delete;

private:
    PluginEnvironment saved_;
};
} // namespace ui::plugin
