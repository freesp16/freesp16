#include "engine/midi/CcMap.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxSlotBank.h"

#include <string>

namespace engine::midi
{
void CcMap::build(const fx::FxSlotBank& bank, const std::function<core::ParamId(std::string_view)>& resolve) // rt-ok: set-up
{
    pages_ = {};
    const auto types = bank.insertTypes();
    types_ = static_cast<int>(types.size() < static_cast<std::size_t>(kMaxTypes) ? types.size() : kMaxTypes);
    if (! resolve)
        return;
    static constexpr std::string_view kUnits[2] = {"fx1", "fx2"};
    for (int u = 0; u < 2; ++u)
        for (int t = 0; t < types_; ++t)
        {
            const auto* d = types[static_cast<std::size_t>(t)];
            if (d == nullptr)
                continue;
            for (int n = 0; n < 6; ++n)
            {
                const std::string_view id = d->params[static_cast<std::size_t>(n)];
                if (id.empty())
                    continue; // an empty knob: CC ignored (FX-086)
                const auto slotId = fx::slotParamId(kUnits[u], id);
                const std::string s(slotId.begin(), slotId.end()); // rt-ok: message-thread set-up only
                auto& p = pages_[static_cast<std::size_t>(u)][static_cast<std::size_t>(t)][static_cast<std::size_t>(n)];
                p.free = resolve(s);
                p.div = resolve(s + ".div"); // rt-ok: set-up only (invalid when the knob has no SYNC form)
            }
        }
}

CcMap::Result CcMap::apply(int scene, int track, int cc, int value, const FxCcContext& fx, ParamSink& sink,
                           ParamChange& out) const noexcept
{
    if (table_ == nullptr || cc < 0 || cc > 127)
        return Result::Ignored;
    const core::midi::CcEntry& e = table_->cc[static_cast<std::size_t>(cc)];
    const auto scope = static_cast<std::uint16_t>(((scene & 15) << 4) | (track & 15));
    switch (e.kind)
    {
        case core::midi::CcKind::None: return Result::Ignored;
        case core::midi::CcKind::SceneSelect: return Result::SceneSelect;
        case core::midi::CcKind::PatternSelect: return Result::PatternSelect;
        case core::midi::CcKind::AllNotesOff: return Result::AllNotesOff;
        case core::midi::CcKind::Linear:
        case core::midi::CcKind::Bands:
        {
            if (! e.param.valid())
                return Result::Ignored;
            const float v = e.kind == core::midi::CcKind::Linear
                                ? core::midi::ccLinear(e, value)
                                : e.lo + static_cast<float>(core::midi::ccBand(e.bands, value));
            out = ParamChange {e.param, scope, v, 0};
            sink.applyParam(out);
            return Result::Applied;
        }
        case core::midi::CcKind::Frames:
        {
            const std::int64_t frames = fx.sampleFrames(track);
            if (! e.param.valid() || frames <= 0)
                return Result::Ignored; // no sample: no length to scale onto
            out = ParamChange{e.param, scope, core::midi::ccFrames(e, value, frames), 0};
            sink.applyParam(out);
            return Result::Applied;
        }
        case core::midi::CcKind::Fx1:
        case core::midi::CcKind::Fx2:
        {
            const int unit = e.kind == core::midi::CcKind::Fx1 ? 0 : 1;
            const int type = fx.insertType(track, unit);
            if (type < 0 || type >= types_)
                return Result::Ignored; // Off slot (FX-086)
            const FxPage& p = page(unit, type, e.bands);
            const core::ParamId id = p.div.valid() && fx.insertSync(track, unit, type) ? p.div : p.free;
            if (! id.valid() || id.value >= core::midi::CcTable::kMaxParams)
                return Result::Ignored; // empty position
            const core::midi::FxGrid g = table_->fxGrid[id.value];
            if (g.hi - 1 - g.lo <= 0)
                return Result::Ignored;
            out = ParamChange {id, scope, core::midi::ccFxValue(value, g), 0};
            sink.applyParam(out);
            return Result::Applied;
        }
    }
    return Result::Ignored;
}
} // namespace engine::midi
