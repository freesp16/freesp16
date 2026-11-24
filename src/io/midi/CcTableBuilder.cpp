#include "io/midi/CcTableBuilder.h"

#include "core/params/catalogue/fx.h"

#include <string>

namespace io::midi
{
void buildCcTable(const core::ParamRegistry& registry, core::midi::CcTable& out)
{
    out = {};
    for (int cc = 0; cc < 128; ++cc)
    {
        auto& e = out.cc[static_cast<std::size_t>(cc)];
        if (cc == 0)
            e.kind = core::midi::CcKind::SceneSelect;
        else if (cc == 32)
            e.kind = core::midi::CcKind::PatternSelect;
        else if (cc == 123)
            e.kind = core::midi::CcKind::AllNotesOff;
        else if (cc >= 46 && cc <= 51)
        {
            e.kind = core::midi::CcKind::Fx1;
            e.bands = static_cast<std::uint8_t>(cc - 46);
        }
        else if (cc >= 63 && cc <= 68)
        {
            e.kind = core::midi::CcKind::Fx2;
            e.bands = static_cast<std::uint8_t>(cc - 63);
        }
        else if (const auto id = registry.byMidiCc(cc))
        {
            const core::ParamSpec& s = registry[*id];
            e.param = *id;
            e.lo = s.minValue;
            e.hi = s.maxValue;
            if (s.type == core::ParamType::Enum || s.type == core::ParamType::Bool)
            {
                e.kind = core::midi::CcKind::Bands; // MID-031: equal bands
                const int n = static_cast<int>(s.maxValue - s.minValue + 0.5f) + 1;
                e.bands = static_cast<std::uint8_t>(n < 1 ? 1 : (n > 128 ? 128 : n));
            }
            else if (s.id == "playback.start" || s.id == "playback.loopStart" || s.id == "playback.length")
            {
                e.kind = core::midi::CcKind::Frames; // over the track's sample, not 0…2^24 − 1 frames
                e.integral = true;
            }
            else
            {
                e.kind = core::midi::CcKind::Linear; // MID-031: linear over the full range
                e.bipolar = s.bipolar && s.minValue < 0.0f && s.maxValue > 0.0f; // centred: 64 = 0
                e.integral = s.type != core::ParamType::Continuous || e.bipolar; // PITCH: whole semitones
            }
        }
    }
    // FX knob grids by ParamId: "fx1.<effect>.<key>[.div]" / "fx2.…" (Insert slots: the only ones with fixed CCs, FX-088).
    const auto all = registry.all();
    for (std::size_t i = 0; i < all.size() && i < core::midi::CcTable::kMaxParams; ++i)
    {
        std::string_view id = all[i].id;
        if (id.substr(0, 4) != "fx1." && id.substr(0, 4) != "fx2.")
            continue;
        const bool div = id.size() > 4 && id.substr(id.size() - 4) == ".div";
        std::string_view rest = id.substr(4);
        if (div)
            rest = rest.substr(0, rest.size() - 4);
        const auto dot = rest.find('.');
        if (dot == std::string_view::npos)
            continue;
        const auto* set = core::fx::effectParams(rest.substr(0, dot));
        if (set == nullptr)
            continue;
        const std::string_view key = rest.substr(dot + 1);
        for (const auto& d : set->params)
        {
            if (d.key != key)
                continue;
            core::fx::Grid g = d.free;
            if (div)
                g = d.sync.span() > 0 ? d.sync : core::fx::syncGrid(d.family);
            out.fxGrid[i] = {g.lo, g.hi};
            break;
        }
    }
}
} // namespace io::midi
