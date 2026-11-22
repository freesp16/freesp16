// The document model root (01-architecture/data-model.md "Entity tree"): plain value types owned by the message
// thread, mutated only through the CommandBus (core/commands/Command.h, EDT-U1), never read by the engine (the
// EngineBridge translates; module-layout.md). Invariants (data-model.md): 16 scenes × 16 tracks × 16 patterns always
// exist, 64 × 16 steps always exist, every SampleKey resolves to a pool entry (possibly missing) — fixed-size arrays
// make the first two true by construction; validate() checks the rest.
//
// Every entity has a default constructor and an `extras` bag; makeDefaultProject, validate, the enums' strings and
// the paths live in .cpp files beside these headers. A Project is ≈ 2 MB: allocate it on the heap.
#pragma once

#include "core/model/Extras.h"
#include "core/model/Sample.h"
#include "core/model/Scene.h"
#include "core/model/enums/Enums.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace core::model
{
struct ProjectSettings // ProjectData (20 fields)
{
    int masterVolume = 100, headphoneVolume = 100, sendTrackVolume = 100, monitorVolume = 0;
    bool afsEnabled = false;
    AfsRouteMode afsRouteMode = AfsRouteMode::Off;
    int afsSendMode = 0, afsSendTrack = -1;
    bool softLimiter = false;
    bool playArrangement = false;
    int swing = 50;          // 50–75 %
    double tempo = 120.0;    // BPM
    QuantizeUnit patternQuantize = QuantizeUnit::Pattern; // M p.42
    QuantizeUnit padQuantize = QuantizeUnit::Off;
    bool padStartsSequence = false;
    int currentScene = 0, currentPattern = 0;
    int formatVersion = 1, formatMinorVersion = 0;
    Extras extras {};
    friend bool operator== (const ProjectSettings&, const ProjectSettings&) = default;
};

struct Section // arrangement row (ARR); the reference's 8-byte record is unknown (OQ-02): kept in extras on import
{
    std::uint8_t scene = 0, pattern = 0;
    int repeats = 1;
    std::optional<double> fixedBpm {};
    Extras extras {};
    friend bool operator== (const Section&, const Section&) = default;
};

struct Arrangement
{
    static constexpr int kMaxSections = 999;
    std::vector<Section> sections;
    int length = 0;
    Extras extras {};
    friend bool operator== (const Arrangement&, const Arrangement&) = default;
};

struct RibbonItem
{
    std::string target {}; // ParamSpec id ("" = none)
    double top = 1.0, bottom = 0.0;
    friend bool operator== (const RibbonItem&, const RibbonItem&) = default;
};

struct RibbonSettings // RIB
{
    std::array<std::array<RibbonItem, 8>, 2> user {}; // USER1, USER2
    int pitchTopCents = 1200, pitchBottomCents = -1200;
    Extras extras {};
    friend bool operator== (const RibbonSettings&, const RibbonSettings&) = default;
};

struct MidiPortSettings
{
    // utility.md §Rows: every enable defaults to ENABLE (requested default; schema 2 — older files keep
    // their values through the v1 → v2 migration, services/store/MigrationV2.cpp).
    bool clock = true, startStop = true, noteCc = true, songPosition = true;
    std::array<int, 16> channels {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}; // per track
    friend bool operator== (const MidiPortSettings&, const MidiPortSettings&) = default;
};

struct MidiSettings // per project; device selections are global preferences (settings.md)
{
    MidiPortSettings input {}, output {};
    Extras extras {};
    friend bool operator== (const MidiSettings&, const MidiSettings&) = default;
};

struct Project
{
    ProjectSettings settings {};
    std::array<Scene, kScenes> scenes; // no {}: GCC 11 expands it element-wise
    Arrangement arrangement {};
    RibbonSettings ribbon {};
    MidiSettings midi {};
    SamplePool sampleRefs {};
    Extras extras {};
    friend bool operator== (const Project&, const Project&) = default;
};
} // namespace core::model
