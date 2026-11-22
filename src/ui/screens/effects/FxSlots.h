// The four effect slots a screen can edit (screens/effects.md "Purpose"; 02-features/effects/README.md FX-001…011) and
// what the effect UI needs to know about them that the frozen core interfaces do not carry:
//   - which effect a slot holds, its bypass and the scene's FX mode (FxSlotAccess). The app installs ModelFxSlots
//     (ModelFxSlots.h: the model's EffectSlot through the SelectFxType / SetFxBypass / SetFxMode commands);
//     LocalFxSlots keeps the state in memory (fixed arrays, no allocation) for tests and as the default;
//   - the live effect telemetry (IN / GR / OUT, ducker envelope, trigger flash; FxMeterView, the panel's view of
//     engine::fx::FxMeterFeed — ModelFxSlots.h EngineFxMeters converts it — read once per frame through
//     FxEnvironment::meters);
//   - the registries the panel and SELECT FX are generated from (UID-215…217). Tests and the bench install their own
//     registries with ScopedFxEnvironment; the app uses the built-in ones.
// Message thread only.
#pragma once

#include "engine/fx/EffectRegistry.h"
#include "ui/descriptors/EffectSpec.h"
#include "ui/shell/FeatureFlags.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ui
{
class PanelHintsRegistry;
}

namespace ui::fx
{
enum class Slot : std::uint8_t
{
    Fx1,
    Fx2,
    Send,
    Master
};
enum class FxMode : std::uint8_t
{
    Both,
    Send,
    Master
}; // FX-008/009: BOTH is the default

constexpr bool isInsert(Slot s) noexcept
{
    return s == Slot::Fx1 || s == Slot::Fx2;
}
// The ParamSpec prefix of a slot (screens/effects.md: <slot>.<effect>.<param>).
constexpr std::string_view prefix(Slot s) noexcept
{
    constexpr std::string_view p[] = {"fx1", "fx2", "send", "master"};
    return p[static_cast<int>(s)];
}
constexpr engine::fx::SlotKind kind(Slot s) noexcept
{
    return isInsert(s) ? engine::fx::SlotKind::Insert
                       : (s == Slot::Send ? engine::fx::SlotKind::Send : engine::fx::SlotKind::Master);
}
// Knob-strip tab and SELECT FX title note (screens/effects.md "Knob strip").
constexpr std::string_view tab(Slot s) noexcept
{
    constexpr std::string_view t[] = {"FX1", "FX2", "SEND FX", "MASTER FX"};
    return t[static_cast<int>(s)];
}
// True when the slot runs in FX mode `m` (FX-008: SEND disables Master, MASTER disables Send).
constexpr bool active(Slot s, FxMode m) noexcept
{
    return isInsert(s) || m == FxMode::Both || (s == Slot::Send) == (m == FxMode::Send);
}
constexpr std::string_view modeName(FxMode m) noexcept
{
    constexpr std::string_view n[] = {"BOTH", "SEND", "MASTER"};
    return n[static_cast<int>(m)];
}

// The id NONE carries in the slot state; an empty id means the same (FX-091: a fresh slot is Off).
inline constexpr std::string_view kNone = "none";
constexpr bool isNone(std::string_view id) noexcept
{
    return id.empty() || id == kNone;
}

// Slot state as the effect screens see it. `scene` and `track` are 1-based; bus slots ignore `track`.
class FxSlotAccess
{
public:
    virtual ~FxSlotAccess() = default;
    virtual std::string_view effect(Slot, int scene, int track) const = 0;                // registered id or "none"
    virtual void selectEffect(Slot, int scene, int track, std::string_view effectId) = 0; // SelectFxType (FX-041)
    virtual bool bypass(Slot, int scene, int track) const = 0;
    virtual void setBypass(Slot, int scene, int track, bool on) = 0; // SetFxBypass (FX-044)
    virtual FxMode mode(int scene) const = 0;
    virtual void setMode(int scene, FxMode) = 0;        // SetFxMode (FX-008)
    virtual std::uint32_t changes() const noexcept = 0; // bumps on every write (the panel rebinds on a change)
};

// In-memory slot state (16 scenes × 16 tracks × FX1/FX2, 16 scenes × SEND/MASTER). `effectId` must have static
// storage (a descriptor id). Fresh slots are NONE, not bypassed, FX mode BOTH.
class LocalFxSlots final : public FxSlotAccess
{
public:
    static LocalFxSlots& shared() noexcept; // the default instance (no ModelFxSlots installed)
    std::string_view effect(Slot, int scene, int track) const override;
    void selectEffect(Slot, int scene, int track, std::string_view effectId) override;
    bool bypass(Slot, int scene, int track) const override;
    void setBypass(Slot, int scene, int track, bool on) override;
    FxMode mode(int scene) const override;
    void setMode(int scene, FxMode) override;
    std::uint32_t changes() const noexcept override { return changes_; }
    void reset() noexcept; // every slot NONE, BOTH (tests)

private:
    struct Entry
    {
        std::string_view effect = kNone;
        bool bypass = false;
    };
    Entry& at(Slot, int scene, int track) noexcept;
    const Entry& at(Slot, int scene, int track) const noexcept;
    std::array<std::array<std::array<Entry, 16>, 16>, 2> inserts_{}; // [fx1/fx2][scene][track]
    std::array<std::array<Entry, 16>, 2> bus_{};                     // [send/master][scene]
    std::array<FxMode, 16> modes_{};
    std::uint32_t changes_ = 0;
};

// Live effect telemetry of one slot (the panel's view of engine::fx::FxMeterFeed). Linear 0–1 levels.
struct FxMeterView
{
    std::array<float, 2> in{}, out{};
    float gainReduction = 0;     // Compressor GR, 0 = none … 1 = full scale of the GR bar
    float duckEnvelope = 0;      // Ducker gain reduction now, 0 … 1 (= DEPTH at full duck)
    double lastTriggerMs = -1e9; // Ducker: the last source trigger (chip flash)
};
using FxMeterSource = const FxMeterView* (*)(Slot, int track, void* user);

struct FxEnvironment
{
    const engine::fx::EffectRegistry* effects = nullptr; // null = EffectRegistry::builtin()
    const EffectUiRegistry* effectUi = nullptr;          // null = EffectUiRegistry::builtin()
    const PanelHintsRegistry* hints = nullptr;           // null = PanelHintsRegistry::builtin()
    FxSlotAccess* slots = nullptr;                       // null = LocalFxSlots::shared()
    FeatureFlags flags{};                                // "fx.extensions" and per-effect flags (UID-833)
    FxMeterSource meters = nullptr;                      // null = no live telemetry (visuals draw their idle state)
    void* metersUser = nullptr;

    const engine::fx::EffectRegistry& effectRegistry() const noexcept;
    const EffectUiRegistry& uiRegistry() const noexcept;
    const PanelHintsRegistry& hintsRegistry() const noexcept;
    FxSlotAccess& slotAccess() const noexcept;
};

FxEnvironment& environment() noexcept; // process-wide, message thread
// The bus slot the MASTER/SEND FX page (mixer.busFx) shows: the page it last showed (SEND first); MIXER's FX SETTING
// sets it before opening the page on the row that was tapped. Message thread.
Slot& busPage() noexcept;

// Installs `env` for the lifetime of the object and restores the previous one (tests, bench).
class ScopedFxEnvironment
{
public:
    explicit ScopedFxEnvironment(const FxEnvironment& env) : saved_(environment()) { environment() = env; }
    ~ScopedFxEnvironment() { environment() = saved_; }
    ScopedFxEnvironment(const ScopedFxEnvironment&) = delete;
    ScopedFxEnvironment& operator=(const ScopedFxEnvironment&) = delete;

private:
    FxEnvironment saved_;
};
} // namespace ui::fx
