// The fixed incoming CC map on the audio thread (spec 02-features/midi.md MID-023/030/031/035, scenes-and-patterns.md
// SCN-050/051, effects/parameters.md FX-086). The table itself is plain data
// (core/util/midi/CcTable.h) built on the message thread from the parameter registry; this class applies one CC to
// one track: a ParamChange through the engine's own message path (so it lands in the engine at once, block start),
// mirrored to the UI by the caller.
//
// FX page positions (MID-035, FX-086): CC 46–51 / 63–68 address page position n of FX1 / FX2 of the track's current
// effect. The position's knob is the effect descriptor's params[n] ("fx.<effect>.<key>" → "fx1.<effect>.<key>", or
// its ".div" SYNC form when the slot's SYNC option is on), mapped onto that knob's current grid; an empty position or
// an Off slot is ignored. The position → ParamId table is built at engine set-up from the FX bank's insert types.
#pragma once

#include "core/util/midi/CcTable.h"
#include "engine/EngineApi.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::fx
{
class FxSlotBank;
}

namespace engine::midi
{
// What CcMap needs of the engine's FX state (implemented by engine::Engine; one virtual call per FX CC).
class FxCcContext
{
public:
    virtual ~FxCcContext() = default;
    virtual int insertType(int track, int unit) const noexcept = 0;           // −1 = Off
    virtual bool insertSync(int track, int unit, int type) const noexcept = 0; // the slot's SYNC option
};

// Where an applied CC goes (the engine's message path).
class ParamSink
{
public:
    virtual ~ParamSink() = default;
    virtual void applyParam(const ParamChange&) noexcept = 0;
};

class CcMap
{
public:
    static constexpr int kMaxTypes = 32;
    struct FxPage
    {
        core::ParamId free {}, div {}; // invalid = empty position
    };

    // Message thread (set-up): the page-position table from the FX bank's insert types and the registry ids.
    void build(const fx::FxSlotBank& bank, const std::function<core::ParamId(std::string_view)>& resolve); // rt-ok: set-up
    void setTable(const core::midi::CcTable* t) noexcept { table_ = t; }
    const core::midi::CcTable* table() const noexcept { return table_; }

    enum class Result : std::uint8_t { Ignored, Applied, SceneSelect, PatternSelect, AllNotesOff };
    // Audio thread: CC `cc` = `value` for `track` (scene = the current scene). `out` = the ParamChange applied.
    Result apply(int scene, int track, int cc, int value, const FxCcContext& fx, ParamSink& sink,
                 ParamChange& out) const noexcept;
    const FxPage& page(int unit, int type, int pos) const noexcept
    {
        return pages_[static_cast<std::size_t>(unit & 1)][static_cast<std::size_t>(type & (kMaxTypes - 1))]
                     [static_cast<std::size_t>(pos < 0 ? 0 : (pos > 5 ? 5 : pos))];
    }

private:
    const core::midi::CcTable* table_ = nullptr;
    std::array<std::array<std::array<FxPage, 6>, kMaxTypes>, 2> pages_ {};
    int types_ = 0;
};
} // namespace engine::midi
