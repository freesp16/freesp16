// Placeholder ParamSpecs for ids the ui::m2 screens bind (knob strips, the BPM/QUANTIZE panel, the SCENE MANAGER
// columns) whose catalogue files may be missing from a build (project.cpp) and for the RENDER AUDIO columns
// (render.cpp). Same rules as M1ParamStubs.cpp: nothing here edits those files.
//   - The catalogue runs last (order 9910) and adds a stub only for an id nobody registered, so a real ParamSpec
//     wins and the stub disappears without an edit (no duplicate id).
//   - Every stub carries featureFlag "ui.m2.paramStub": the shell shows no column for a flagged spec while the flag is
//     off; the ui::m2 tests turn the flag on.
//   - Stubs are never lockable, LFO targets or preferences and carry no MIDI CC (the generated views stay the real
//     catalogues'): without the sequencer catalogue, a held step shows the SEQUENCE columns 1–3 disabled.
// Also registered here, for good (not stubs): the SCENE MANAGER's UI-state columns `sceneManager.*`
// (scene-manager.md: SEQ. and TRACK tab knobs, "UI state, not saved").
// Delete the stub part once the five catalogues all exist (tests/snapshot/m2/test_m2_registry.cpp lists them).
#include "core/params/ParamRegistry.h"
#include "ui/screens/sequence/M2Session.h"

#include <cmath>
#include <cstdio>

#if defined(__GNUC__) && ! defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace ui::m2stubs
{
namespace
{
using core::ParamScope;
using core::ParamType;

constexpr std::string_view offOn[] = {"OFF", "ON"};
constexpr std::string_view monitor[] = {"Master + Headphones", "Master", "Headphones"};
constexpr std::string_view preCount[] = {"OFF", "1 BAR", "2 BARS"};
constexpr std::string_view renderLength[] = {"1\xC3\x97", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97"};
constexpr std::string_view bitDepth[] = {"16-bit", "24-bit", "32-bit float"};
constexpr std::string_view numbers[] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                        "9", "10", "11", "12", "13", "14", "15", "16"};
constexpr std::string_view trackMode[] = {"CURRENT", "ALL"};
constexpr std::string_view barLength[] = {"1 Bar", "2 Bars", "3 Bars", "4 Bars"};

int formatSigned(float v, char* buf, int n) noexcept
{
    const long i = std::lround(v);
    return std::snprintf(buf, static_cast<std::size_t>(n), i > 0 ? "+%ld" : "%ld", i);
}
int formatBpm(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", static_cast<double>(v));
}
int formatPercent(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld %%", std::lround(v));
}
void stub(core::ParamRegistry& r, core::ParamSpec s)
{
    if (r.find(s.id) != nullptr)
        return;
    s.featureFlag = ui::m2::kParamStubFlag;
    s.lockable = false;
    s.lfoTarget = false;
    s.stripTarget = false;
    s.midiCc = -1;
    if (s.shortLabel.empty())
        s.shortLabel = s.label.substr(0, 8);
    r.add(s);
}

void own(core::ParamRegistry& r, core::ParamSpec s)
{
    if (r.find(s.id) != nullptr)
        return;
    if (s.shortLabel.empty())
        s.shortLabel = s.label.substr(0, 8);
    r.add(s);
}

void addStubs(core::ParamRegistry& r)
{
    // transport.cpp
    stub(r, {.id = "transport.tempo",
             .label = "BPM",
             .group = "TRANSPORT",
             .type = ParamType::Continuous,
             .minValue = 20,
             .maxValue = 300,
             .defaultValue = 120,
             .coarseStep = 1,
             .fineStep = 0.1f,
             .format = formatBpm,
             .scope = ParamScope::Project});
    stub(r, {.id = "transport.metronome",
             .label = "METRONOME",
             .group = "TRANSPORT",
             .type = ParamType::Enum,
             .maxValue = 1,
             .enumLabels = offOn,
             .scope = ParamScope::Project});
    stub(r, {.id = "transport.metronomeVolume",
             .label = "METRONOME VOLUME",
             .group = "TRANSPORT",
             .type = ParamType::Integer,
             .maxValue = 127,
             .defaultValue = 100,
             .scope = ParamScope::Project});
    stub(r, {.id = "transport.metronomeMonitor",
             .label = "METRONOME MONITOR",
             .group = "TRANSPORT",
             .type = ParamType::Enum,
             .maxValue = 2,
             .enumLabels = monitor,
             .scope = ParamScope::Project});
    stub(r, {.id = "transport.preCount",
             .label = "PRE-COUNT",
             .group = "TRANSPORT",
             .type = ParamType::Enum,
             .maxValue = 2,
             .enumLabels = preCount,
             .scope = ParamScope::Project});
    stub(r, {.id = "transport.nudge",
             .label = "NUDGE",
             .group = "TRANSPORT",
             .type = ParamType::Integer,
             .minValue = -1,
             .maxValue = 1,
             .format = formatSigned,
             .bipolar = true,
             .scope = ParamScope::Project});
    stub(r, {.id = "transport.nudgeAmount",
             .label = "NUDGE AMOUNT",
             .group = "TRANSPORT",
             .type = ParamType::Integer,
             .minValue = 1,
             .maxValue = 10,
             .defaultValue = 3,
             .format = formatPercent,
             .scope = ParamScope::Project});

    // sequencer.cpp exists: its ParamSpecs are the real ones (no stubs).

    // scene.cpp: real, core/params/catalogue/scene.cpp.

    // project.cpp
    stub(r, {.id = "project.resume",
             .label = "RESUME",
             .group = "PROJECT",
             .type = ParamType::Enum,
             .maxValue = 1,
             .defaultValue = 1,
             .enumLabels = offOn,
             .scope = ParamScope::Project});
    stub(r, {.id = "project.autosave",
             .label = "AUTOSAVE",
             .group = "PROJECT",
             .type = ParamType::Enum,
             .maxValue = 1,
             .defaultValue = 1,
             .enumLabels = offOn,
             .scope = ParamScope::Project});

    // render.cpp: RENDER AUDIO strip (project.md, UID-860)
    stub(r, {.id = "render.length",
             .label = "LENGTH",
             .group = "RENDER",
             .type = ParamType::Enum,
             .maxValue = 3,
             .enumLabels = renderLength,
             .scope = ParamScope::Project});
    stub(r, {.id = "render.bitDepth",
             .label = "BIT DEPTH",
             .group = "RENDER",
             .type = ParamType::Enum,
             .maxValue = 2,
             .defaultValue = 1,
             .enumLabels = bitDepth,
             .scope = ParamScope::Project});
    stub(r, {.id = "render.normalize",
             .label = "NORMALIZE",
             .group = "RENDER",
             .type = ParamType::Enum,
             .maxValue = 1,
             .enumLabels = offOn,
             .scope = ParamScope::Project});

    // SCENE MANAGER UI state (scene-manager.md "SEQ." and "TRACK" knob strips; not saved: scope Project with a
    // "ui." storage key the store skips).
    own(r, {.id = "sceneManager.seq.scene",
            .label = "SCENE No.",
            .group = "SCENE MANAGER",
            .type = ParamType::Enum,
            .maxValue = 15,
            .enumLabels = numbers,
            .scope = ParamScope::Project,
            .storage = {.key = "ui.sm.scene"}});
    own(r, {.id = "sceneManager.seq.pattern",
            .label = "PATTERN No.",
            .group = "SCENE MANAGER",
            .type = ParamType::Enum,
            .maxValue = 15,
            .enumLabels = numbers,
            .scope = ParamScope::Project,
            .storage = {.key = "ui.sm.pattern"}});
    own(r, {.id = "sceneManager.seq.trackMode",
            .label = "TRACK",
            .group = "SCENE MANAGER",
            .type = ParamType::Enum,
            .maxValue = 1,
            .enumLabels = trackMode,
            .scope = ParamScope::Project,
            .storage = {.key = "ui.sm.trackMode"}});
    own(r, {.id = "sceneManager.seq.track",
            .label = "TRACK No.",
            .group = "SCENE MANAGER",
            .type = ParamType::Enum,
            .maxValue = 15,
            .enumLabels = numbers,
            .scope = ParamScope::Project,
            .storage = {.key = "ui.sm.track"}});
    own(r, {.id = "sceneManager.seq.barLength",
            .label = "BAR LENGTH",
            .group = "SCENE MANAGER",
            .type = ParamType::Enum,
            .maxValue = 3,
            .enumLabels = barLength,
            .scope = ParamScope::Project,
            .storage = {.key = "ui.sm.barLength"}});
    own(r, {.id = "sceneManager.track.mode",
            .label = "TRACK",
            .group = "SCENE MANAGER",
            .type = ParamType::Enum,
            .maxValue = 1,
            .enumLabels = trackMode,
            .scope = ParamScope::Project,
            .storage = {.key = "ui.sm.trackTabMode"}});
}
} // namespace
} // namespace ui::m2stubs

APP_REGISTER_PARAMS("ui_m2_stubs", 9910, &ui::m2stubs::addStubs)
