// Test-only effect world for the effect UI (tests without the effect algorithms): descriptors and per-slot
// ParamSpecs that copy the spec's per-effect table (screens/effects.md "The 12 effects": ids, header names, slots,
// option rows, visual kinds, captions in page order) with no DSP, plus GAIN TEST (flag fx.test) and P3 extensions
// (flagged) for EXT badges and paging. Values and formats are fixture data chosen to reproduce the mockups' demo
// values. Everything lives in private registries installed with ScopedFxEnvironment — never in the built-in ones, so
// the real effect registrations never collide with these.
//
// Shared by tests/unit/fxui (logic), tests/snapshot/m3 (mockup comparisons) and tests/bench/bench_ui_effects.
#pragma once

#include "core/params/ParamRegistry.h"
#include "engine/fx/EffectRegistry.h"
#include "ui/descriptors/EffectSpec.h"
#include "ui/screens/effects/FxCatalogue.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/shell/Shell.h"
#include "ui/widgets/EffectPanel.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace fxtest
{
using core::ParamType;
namespace efx = engine::fx;

// --- fixture formatters (they print the mockups' demo texts) ---------------------------------------------------------
inline int fmtPercent(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.0f %%", static_cast<double>(v * 100));
}
inline int fmtHz(float v, char* b, int n) noexcept // 20 Hz … 20 kHz, log
{
    const double hz = 20.0 * std::pow(1000.0, static_cast<double>(v));
    return hz >= 1000 ? std::snprintf(b, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000)
                      : std::snprintf(b, static_cast<std::size_t>(n), "%.1f Hz", hz);
}
inline int fmtDb(float v, char* b, int n) noexcept // bipolar ±12 dB
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.0f dB", static_cast<double>((v - 0.5f) * 24));
}
inline int fmtQ(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.1f", static_cast<double>(0.7f + v * 6));
}
inline int fmtGlide(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.0f", static_cast<double>(v * 100));
}
inline int fmtPreDelay(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.1f ms", static_cast<double>(v * 100));
}
inline int fmtRoom(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.0f", static_cast<double>(std::round(v * 100) + 1));
}
inline int fmtSeconds(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.2f s", static_cast<double>(v * 17.76f));
}
inline int fmtMs(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%.0f ms", static_cast<double>(10 + v * 990));
}

// --- the table
// ---------------------------------------------------------------------------------------------------------
struct KnobDef
{
    std::string_view key, label;
    float def = 0;
    bool bipolar = false;
    core::ParamFormatter format = nullptr;
    std::span<const std::string_view> divisions{}; // SYNC form `<id>.div` (label DIV unless keepCaption)
    bool keepCaption = false;                      // the Ducker keeps ATTACK / RELEASE (UID-830)
};
struct OptionDef
{
    std::string_view key, label;
    std::span<const std::string_view> values{}; // empty = Bool
    std::uint8_t slots = 0xFF;                  // slot kinds the option exists on (LOOK AHEAD: bus only)
};
struct EffectDef
{
    std::string_view id, name;
    std::uint8_t slots;
    std::array<KnobDef, 6> knobs;
    std::vector<OptionDef> options;
    std::string_view visual;
    std::string_view flag{};
};

inline constexpr std::uint8_t kInsert = static_cast<std::uint8_t>(efx::SlotKind::Insert);
inline constexpr std::uint8_t kBus = efx::SlotKind::Send | efx::SlotKind::Master;
inline constexpr std::uint8_t kAll = kInsert | kBus;

inline constexpr std::string_view kChorusDiv[] = {"16", "8", "4", "2", "1", "3/4", "1/2", "1/4", "1/8", "1/16"};
inline constexpr std::string_view kFlangerDiv[] = {"64", "32", "16", "8", "4", "2", "1"};
inline constexpr std::string_view kPhaserDiv[] = {"64", "32",  "16",  "8",   "4",   "2",
                                                  "1",  "3/4", "1/2", "1/4", "1/8", "1/16"};
inline constexpr std::string_view kDelayDiv[] = {"1/8", "1/4", "1/2", "3/4", "1", "2"};
inline constexpr std::string_view kDelayBusDiv[] = {"1/8", "1/4", "1/2", "3/4", "1", "2", "4", "8"};
inline constexpr std::string_view kDuckDiv[] = {"0.25 /16", "0.50 /16", "1.00 /16", "2.00 /16", "4.00 /16"};
inline constexpr std::string_view kFilterTypes[] = {"LPF", "HPF", "BPF", "OFF"};
inline constexpr std::string_view kSlopes[] = {"12 dB/Oct", "24 dB/Oct"};
inline constexpr std::string_view kBandTypes[] = {"HPF", "LPF", "PEAK", "HI-SHELF", "LO-SHELF"};
inline constexpr std::string_view kPrePost[] = {"PRE", "POST"};
inline constexpr std::string_view kPeakRms[] = {"PEAK", "RMS"};
inline constexpr std::string_view kTracks[] = {"TRACK 1",  "TRACK 2",  "TRACK 3",  "TRACK 4",  "TRACK 5",  "TRACK 6",
                                               "TRACK 7",  "TRACK 8",  "TRACK 9",  "TRACK 10", "TRACK 11", "TRACK 12",
                                               "TRACK 13", "TRACK 14", "TRACK 15", "TRACK 16"};
inline constexpr std::string_view kBits[] = {"4 bit", "6 bit", "8 bit", "12 bit", "16 bit"};

inline std::vector<EffectDef> table()
{
    const KnobDef none{};
    auto mix = [](float def = 1.0f) { return KnobDef{"mix", "MIX RATIO", def, false, &fmtPercent}; };
    return {
        {"none", "NONE", kAll, {}, {}, ""},
        {"chorus",
         "CHORUS",
         kAll,
         {KnobDef{"rate", "RATE", 0.3f, false, nullptr, kChorusDiv},
          {"width", "WIDTH", 0.5f},
          {"phase", "PHASE", 0.5f},
          {"delay", "DELAY", 0.5f},
          none,
          mix(0)},
         {{"sync", "SYNC"}},
         "lfoCurve"},
        {"flanger",
         "FLANGER",
         kAll,
         {KnobDef{"rate", "RATE", 0.3f, false, nullptr, kFlangerDiv},
          {"width", "WIDTH", 0.5f},
          {"phase", "PHASE", 0.5f},
          {"delay", "DELAY", 0.5f},
          {"feedback", "FEEDBACK", 0.5f},
          mix(0)},
         {{"sync", "SYNC"}},
         "lfoCurve"},
        {"phaser",
         "PHASER",
         kAll,
         {KnobDef{"rate", "RATE", 0.3f, false, nullptr, kPhaserDiv},
          {"width", "WIDTH", 0.5f},
          {"phase", "PHASE", 0.5f},
          {"frequency", "FREQUENCY", 0.5f},
          {"feedback", "FEEDBACK", 0.5f},
          mix(0)},
         {{"sync", "SYNC"}},
         "lfoCurve"},
        {"eq2",
         "2 BAND EQ",
         kAll,
         {KnobDef{"b1q", "B1 Q", 0.1f},
          {"b1freq", "B1 FREQUENCY", 0.35f},
          {"b1gain", "B1 GAIN", 0.5f, true},
          {"b2q", "B2 Q", 0.1f},
          {"b2freq", "B2 FREQUENCY", 0.77f},
          {"b2gain", "B2 GAIN", 0.5f, true}},
         {{"band1Type", "BAND1 TYPE", kBandTypes}, {"band2Type", "BAND2 TYPE", kBandTypes}},
         "eqCurve"},
        {"filter",
         "MULTI MODE FILTER",
         kAll,
         {KnobDef{"cutoff", "CUT OFF", 0.71f, false, &fmtHz},
          {"gain", "GAIN", 0.5f, true, &fmtDb},
          {"q", "RESONANCE", 0.15f, false, &fmtQ},
          {"glide", "GLIDE", 0, false, &fmtGlide},
          none,
          mix()},
         {{"type", "TYPE", kFilterTypes}, {"slope", "SLOPE", kSlopes}},
         "filterCurve"},
        {"delay",
         "DELAY",
         kInsert,
         {KnobDef{"time", "TIME", 0.2f, false, &fmtMs, kDelayDiv},
          {"feedback", "FEEDBACK", 0.7f, false, &fmtPercent},
          {"hiCut", "HI CUT", 1.0f, false, &fmtHz},
          none,
          none,
          mix(0)},
         {{"sync", "SYNC"}},
         "delayTaps"},
        {"delayBus",
         "DELAY",
         kBus,
         {KnobDef{"time", "TIME", 0.2f, false, &fmtMs, kDelayBusDiv},
          {"feedback", "FEEDBACK", 0.5f, false, &fmtPercent},
          {"hiCut", "HI CUT", 1.0f, false, &fmtHz},
          {"loCut", "LO CUT", 0, false, &fmtHz},
          {"spread", "SPREAD", 0.4f, false, &fmtPercent},
          mix()},
         {{"sync", "SYNC"}},
         "delayTaps"},
        {"reverb",
         "REVERB",
         kBus,
         {KnobDef{"preDelay", "PRE DELAY", 0.12f, false, &fmtPreDelay},
          {"room", "ROOM SIZE", 0.49f, false, &fmtRoom},
          {"time", "TIME", 0.49f, false, &fmtSeconds},
          {"damping", "DAMPING", 0.4f, false, &fmtHz},
          {"spread", "SPREAD", 0.4f, false, &fmtPercent},
          mix()},
         {},
         "decayCurve"},
        {"lofi",
         "LO-FI",
         kAll,
         {KnobDef{"preamp", "PREAMP", 0.2f},
          {"reduction", "REDUCTION", 0.75f},
          {"bits", "BIT DEPTH", 0.55f},
          {"cutoff", "CUT OFF", 0.566f, false, &fmtHz},
          {"resonance", "RESONANCE", 0.05f},
          mix()},
         {{"filterType", "FILTER TYPE", kFilterTypes}, {"filterPosition", "", kPrePost}},
         "none"},
        {"distortion",
         "DISTORTION",
         kAll,
         {KnobDef{"drive", "DRIVE", 0.5f},
          {"loFreq", "LO FREQ", 0.2f},
          {"hiFreq", "HI FREQ", 0.8f},
          {"dynamics", "DYNAMICS", 0.5f},
          {"tone", "TONE", 0.5f},
          mix()},
         {},
         "none"},
        {"compressor",
         "COMPRESSOR",
         kAll,
         {KnobDef{"ratio", "RATIO", 0.1f},
          {"attack", "ATTACK", 0.03f},
          {"release", "RELEASE", 0.03f},
          {"threshold", "THRESHOLD", 0.66f},
          {"gain", "GAIN", 0.12f},
          mix()},
         {{"lookAhead", "LOOK AHEAD", {}, kBus}, {"detect", "DETECT", kPeakRms}, {"knee", "SOFT KNEE"}},
         "meters"},
        {"ducker",
         "DUCKER",
         kInsert,
         {KnobDef{"attack", "ATTACK", 0.03f, false, nullptr, kDuckDiv, true},
          {"attackCurve", "CURVE", 0.5f, true},
          {"release", "RELEASE", 0.04f, false, nullptr, kDuckDiv, true},
          {"releaseCurve", "CURVE", 0.5f, true},
          none,
          {"depth", "DEPTH", 0.85f, false, &fmtPercent}},
         {{"sync", "SYNC"}, {"source", "SOURCE", kTracks}},
         "duckEnvelope"},
        {"gainTest",
         "GAIN TEST",
         kAll,
         {KnobDef{"gain", "GAIN", 0.5f}, none, none, none, none, none},
         {},
         "none",
         "fx.test"},
        // P3 extensions (flagged): EXT badges and the 16th cell that pages the insert grid.
        {"limiter",
         "LIMITER",
         kAll,
         {KnobDef{"ceiling", "CEILING", 0.9f}, none, none, none, none, mix()},
         {},
         "none",
         "fx.limiter"},
        {"ringmod",
         "RING MOD",
         kAll,
         {KnobDef{"freq", "FREQUENCY", 0.5f}, none, none, none, none, mix()},
         {},
         "none",
         "fx.ringmod"},
        {"tremolo",
         "TREMOLO",
         kInsert,
         {KnobDef{"rate", "RATE", 0.5f}, {"depth", "DEPTH", 0.5f}, none, none, none, mix()},
         {{"sync", "SYNC"}},
         "lfoCurve",
         "fx.tremolo"},
        {"autopan",
         "AUTO PAN",
         kInsert,
         {KnobDef{"rate", "RATE", 0.5f}, none, none, none, none, mix()},
         {},
         "lfoCurve",
         "fx.autopan"},
        {"autowah",
         "AUTO WAH",
         kInsert,
         {KnobDef{"rate", "RATE", 0.5f}, none, none, none, none, mix()},
         {},
         "filterCurve",
         "fx.autowah"},
    };
}

// The mockups' visual values (adapters of the test effects).
inline void filterAdapter(const ui::EffectVisualInputs& in, ui::VisualModel& m) noexcept
{
    ui::defaultVisualAdapter("filterCurve")(in, m);
    m.cutoffHz = 20.0f * std::pow(1000.0f, in.knob01[0]);
    m.resonance = 0.7f + in.knob01[2] * 6;
    m.filterGainDb = 0;
}
inline void delayAdapter(const ui::EffectVisualInputs& in, ui::VisualModel& m) noexcept
{
    ui::defaultVisualAdapter("delayTaps")(in, m);
    if (in.sync)
    {
        static constexpr float beats[] = {0.125f, 0.25f, 0.5f, 0.75f, 1, 2};
        m.tapFraction = beats[std::clamp(static_cast<int>(std::lround(in.knob01[0] * 5)), 0, 5)] / 4; // 1 bar window
    }
}
inline void reverbAdapter(const ui::EffectVisualInputs& in, ui::VisualModel& m) noexcept
{
    ui::defaultVisualAdapter("decayCurve")(in, m);
    m.preDelayFraction = in.knob01[0] * 0.25f;
    m.lengthFraction = in.knob01[2] * 1.53f;
}

class World
{
public:
    static World& get()
    {
        static World w;
        return w;
    }
    core::ParamRegistry params;
    efx::EffectRegistry effects;
    ui::EffectUiRegistry effectUi;
    ui::PanelHintsRegistry hints;
    ui::fx::LocalFxSlots slots;
    std::vector<EffectDef> defs;

    ui::fx::FxEnvironment environment(bool extensions = false)
    {
        ui::fx::FxEnvironment e;
        e.effects = &effects;
        e.effectUi = &effectUi;
        e.hints = &hints;
        e.slots = &slots;
        if (extensions)
            e.flags.enable(ui::fx::kExtensionsFlag);
        return e;
    }
    const core::ParamSpec& spec(std::string_view id) const
    {
        const auto* s = params.find(id);
        if (s == nullptr)
            std::fprintf(stderr, "fxtest: no ParamSpec %.*s\n", static_cast<int>(id.size()), id.data());
        return *s;
    }

private:
    std::deque<std::string> strings_;
    std::deque<std::vector<std::string_view>> optionLists_;

    std::string_view keep(std::string s)
    {
        strings_.push_back(std::move(s));
        return strings_.back();
    }

    World() : params(core::builtinParams()), defs(table())
    {
        static constexpr std::string_view kSlotNames[] = {"fx1", "fx2", "send", "master"};
        static constexpr std::uint8_t kSlotKinds[] = {kInsert, kInsert, static_cast<std::uint8_t>(efx::SlotKind::Send),
                                                      static_cast<std::uint8_t>(efx::SlotKind::Master)};
        for (const auto& d : defs)
        {
            std::array<std::string_view, 6> knobIds{};
            for (std::size_t n = 0; n < 6; ++n)
                if (! d.knobs[n].key.empty())
                    knobIds[n] = keep("fx." + std::string(d.id) + "." + std::string(d.knobs[n].key));
            auto& options = optionLists_.emplace_back();
            for (const auto& o : d.options)
                options.push_back(keep("fx." + std::string(d.id) + "." + std::string(o.key)));
            for (std::size_t s = 0; s < 4; ++s)
            {
                if ((d.slots & kSlotKinds[s]) == 0)
                    continue;
                const bool insert = s < 2;
                const auto prefix = std::string(kSlotNames[s]) + "." + std::string(d.id) + ".";
                for (const auto& k : d.knobs)
                {
                    if (k.key.empty())
                        continue;
                    core::ParamSpec p;
                    p.id = keep(prefix + std::string(k.key));
                    p.label = k.label;
                    p.shortLabel = k.label;
                    p.group = d.name;
                    p.defaultValue = k.def;
                    p.coarseStep = 0.01f;
                    p.fineStep = 0.001f;
                    p.bipolar = k.bipolar;
                    p.format = k.format;
                    p.lockable = insert;
                    p.lfoTarget = insert;
                    p.stripTarget = insert;
                    p.scope = insert ? core::ParamScope::Track : core::ParamScope::Scene;
                    params.add(p);
                    if (! k.divisions.empty())
                    {
                        core::ParamSpec div = p;
                        div.id = keep(prefix + std::string(k.key) + ".div");
                        div.label = k.keepCaption ? k.label : "DIV";
                        div.type = ParamType::Enum;
                        div.minValue = 0;
                        div.maxValue = static_cast<float>(k.divisions.size() - 1);
                        div.defaultValue = 1;
                        div.coarseStep = 1;
                        div.fineStep = 1;
                        div.format = nullptr;
                        div.enumLabels = k.divisions;
                        params.add(div);
                    }
                }
                for (const auto& o : d.options)
                {
                    if ((o.slots & kSlotKinds[s]) == 0)
                        continue;
                    core::ParamSpec p;
                    p.id = keep(prefix + std::string(o.key));
                    p.label = o.label.empty() ? std::string_view("POSITION") : o.label;
                    p.shortLabel = p.label;
                    p.group = d.name;
                    p.type = o.values.empty() ? ParamType::Bool : ParamType::Enum;
                    p.maxValue = o.values.empty() ? 1.0f : static_cast<float>(o.values.size() - 1);
                    p.coarseStep = 1;
                    p.fineStep = 1;
                    p.enumLabels = o.values;
                    p.scope = insert ? core::ParamScope::Track : core::ParamScope::Scene;
                    params.add(p);
                }
            }
            efx::EffectDescriptor desc{};
            desc.id = d.id;
            desc.name = d.name;
            desc.slots = d.slots;
            desc.params = knobIds;
            desc.options = options;
            desc.visual = d.visual;
            desc.featureFlag = d.flag;
            effects.add(desc);
        }
        hints.add("filter", ui::PanelHints{.visual = &filterAdapter, .gridLabel = "FILTER"});
        hints.add("delay", ui::PanelHints{.visual = &delayAdapter});
        hints.add("reverb", ui::PanelHints{.visual = &reverbAdapter});
        hints.add("lofi", ui::PanelHints{.bracketOption = 1, .bracketKnobMask = 0b11000});
        static constexpr ui::EffectUiSpec busDelay{.effectId = "delayBus", .icon = "delay", .options = {}};
        effectUi.add(busDelay);
    }
};

// A Shell on a private screen registry holding the effect screens and the world's parameters.
struct FxShell
{
    ui::ScreenRegistry screens;
    std::unique_ptr<ui::Shell> shell;
    ui::DrawList dl;
    double now = 10000;

    explicit FxShell(const ui::TextMeasure& measure)
    {
        for (const auto id : {"track.fx1", "track.fx2", "mixer.busFx", "track.menu", "mixer"})
            if (const auto* s = ui::ScreenRegistry::builtin().find(id))
                screens.add(*s);
        ui::ShellConfig cfg{World::get().params, measure};
        cfg.screens = &screens;
        shell = std::make_unique<ui::Shell>(std::move(cfg));
        dl.reserve(8192, 65536);
        settle();
    }
    ui::DrawList& frame(double advanceMs = 16)
    {
        now += advanceMs;
        dl.clear();
        shell->frame(dl, now);
        return dl;
    }
    ui::DrawList& settle()
    {
        for (int i = 0; i < 2; ++i)
            frame(1000);
        return dl;
    }
    ui::EffectPanel* panel() { return dynamic_cast<ui::EffectPanel*>(shell->body()); }
    void send(io::ControlEvent e)
    {
        e.timeMs = now;
        shell->dispatch(e);
    }
    void press(io::Control c, int v = 100)
    {
        send({io::ControlEvent::Kind::Press, c, static_cast<std::int16_t>(v), io::kSourceVirtual, 0});
    }
    void release(io::Control c) { send({io::ControlEvent::Kind::Release, c, 0, io::kSourceVirtual, 0}); }
    void turn(io::Control c, int d)
    {
        send({io::ControlEvent::Kind::Turn, c, static_cast<std::int16_t>(d), io::kSourceVirtual, 0});
    }
    void tap(float cx, float cy)
    {
        io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, ui::touch::kDown, io::kSourceVirtual,
                           now};
        e.x = static_cast<std::int16_t>(cx / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(cy / ui::touch::kToCanvas + 0.5f);
        shell->dispatch(e);
        e.value = ui::touch::kUp;
        shell->dispatch(e);
    }
    float get(std::string_view id)
    {
        const auto& s = World::get().spec(id);
        auto& st = shell->state();
        return shell->values().get(s, ui::fx::scopeOf(s, st.scene, st.selectedTrack));
    }
    void set(std::string_view id, float v)
    {
        const auto& s = World::get().spec(id);
        auto& st = shell->state();
        shell->values().set(s, ui::fx::scopeOf(s, st.scene, st.selectedTrack), v);
    }
    bool drewText(std::string_view text) const
    {
        for (const auto& c : dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == text)
                return true;
        return false;
    }
};
} // namespace fxtest
