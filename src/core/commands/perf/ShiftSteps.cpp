#include "core/commands/perf/ShiftSteps.h"

#include "core/commands/SetParam.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/Project.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace core::perf
{
namespace
{
using model::Pattern;
using model::Step;

constexpr float kNeverRemove = -1.0e30f; // setLocks' removeAt: outside every ParamSpec range, so a lock is always written

Pattern& patternOf(model::Project& p, int scene, int pattern) noexcept
{
    return p.scenes[static_cast<std::size_t>(scene & 15)].patterns[static_cast<std::size_t>(pattern & 15)];
}

// Rotates cells [first, first + count) of `track` by `delta` (positive = later steps), in place.
void rotate(Pattern& p, int track, ShiftRange r, int delta) noexcept
{
    if (r.count < 2)
        return;
    const int d = ((delta % r.count) + r.count) % r.count;
    if (d == 0)
        return;
    std::array<Step, model::kSteps> column; // default member initialisers, no {} (GCC 11)
    const auto t = static_cast<std::size_t>(track & 15);
    for (int i = 0; i < r.count; ++i)
        column[static_cast<std::size_t>(i)] = p.steps[static_cast<std::size_t>(r.first + i)][t];
    for (int i = 0; i < r.count; ++i)
        p.steps[static_cast<std::size_t>(r.first + (i + d) % r.count)][t] = column[static_cast<std::size_t>(i)];
}

class ShiftCommand final : public Command
{
public:
    // Unresolved (area, bar: the range comes from the pattern at apply time) or resolved (the inverse: a fixed range).
    ShiftCommand(int scene, int pattern, int track, ShiftArea area, int bar, int delta) noexcept
        : scene_(scene & 15), pattern_(pattern & 15), track_(track & 15), area_(area), bar_(bar), delta_(delta)
    {
    }
    ShiftCommand(int scene, int pattern, int track, ShiftRange range, int delta) noexcept
        : scene_(scene & 15), pattern_(pattern & 15), track_(track & 15), resolved_(true), range_(range), delta_(delta)
    {
    }

    std::string_view label() const override { return "SHIFT"; }
    void apply(model::Project& p) override
    {
        Pattern& pat = patternOf(p, scene_, pattern_);
        rotate(pat, track_, rangeIn(pat), delta_);
    }
    std::unique_ptr<Command> inverse(const model::Project& before) const override
    {
        const Pattern& pat = patternOf(const_cast<model::Project&>(before), scene_, pattern_);
        return std::make_unique<ShiftCommand>(scene_, pattern_, track_, rangeIn(pat), -delta_);
    }
    // `other` is a newer detent's inverse (run before this one on undo): the two rotations add up (EDT-U2).
    bool mergeWith(const Command& other, double) override
    {
        const auto* o = dynamic_cast<const ShiftCommand*>(&other);
        if (o == nullptr || ! resolved_ || ! o->resolved_ || o->scene_ != scene_ || o->pattern_ != pattern_ ||
            o->track_ != track_ || o->range_.first != range_.first || o->range_.count != range_.count)
            return false;
        delta_ += o->delta_;
        if (range_.count > 0)
            delta_ %= range_.count;
        return true;
    }
    void collectChanges(ChangeSet& c) const override { c.add(commands::sequencer::patternPath(scene_, pattern_)); }
    std::size_t sizeBytes() const override { return sizeof(*this); }

private:
    ShiftRange rangeIn(const Pattern& p) const noexcept { return resolved_ ? range_ : shiftRange(p.length, area_, bar_); }

    int scene_, pattern_, track_;
    bool resolved_ = false;
    ShiftArea area_ = ShiftArea::All;
    int bar_ = 0;
    ShiftRange range_ {};
    int delta_;
};

// The track-level SEQUENCE values (TrackSequencerSettings) by ParamSpec id.
using Field = int& (*) (model::Project&, const model::ParamPath&);
model::TrackSequencerSettings& seqOf(model::Project& p, const model::ParamPath& at) noexcept
{
    return p.scenes[at.scene & 15].tracks[at.track & 15].seq;
}
struct TrackField
{
    std::string_view id;
    Field field;
};
const TrackField kTrackFields[] = {
    {"seq.offset", [](model::Project& p, const model::ParamPath& at) -> int& { return seqOf(p, at).offset; }},
    {"seq.retrigCount", [](model::Project& p, const model::ParamPath& at) -> int& { return seqOf(p, at).retriggerCount; }},
    {"seq.retrigSpeed", [](model::Project& p, const model::ParamPath& at) -> int& { return seqOf(p, at).retriggerSpeed; }},
    {"seq.transpose", [](model::Project& p, const model::ParamPath& at) -> int& { return seqOf(p, at).transpose; }},
    {"seq.shiftArea", [](model::Project& p, const model::ParamPath& at) -> int& { return seqOf(p, at).stepShiftArea; }},
};
} // namespace

std::unique_ptr<Command> shiftSteps(int scene, int pattern, int track, ShiftArea area, int bar, int delta)
{
    return std::make_unique<ShiftCommand>(scene, pattern, track, area, bar, delta);
}

std::unique_ptr<Command> setStepsParam(int scene, int pattern, int track, std::uint64_t stepMask, ParamId param,
                                       float value, const ParamRegistry& registry)
{
    if (! param.valid() || param.value >= registry.size())
        return nullptr;
    const ParamSpec& spec = registry[param];
    const float v = spec.clamp(value);
    if (stepMask == 0)
    {
        for (const auto& f : kTrackFields)
            if (f.id == spec.id)
            {
                char path[64];
                std::snprintf(path, sizeof path, "scene[%d].track[%d].%.*s", scene & 15, track & 15,
                              static_cast<int>(spec.id.size()), spec.id.data());
                const model::ParamPath at {model::Scope::Track, static_cast<std::uint8_t>(scene & 15),
                                           static_cast<std::uint8_t>(track & 15), 0, 0, 0, param};
                return makeSetParam<int>(path, spec.label, f.field, at, static_cast<int>(std::lround(v)));
            }
        return nullptr;
    }
    const int slot = commands::sequencer::lockSlotOf(spec, registry);
    if (slot < 0)
        return nullptr;
    std::vector<int> steps;
    std::vector<float> values;
    for (std::uint64_t m = stepMask; m != 0; m &= m - 1)
    {
        steps.push_back(__builtin_ctzll(m));
        values.push_back(v);
    }
    return commands::sequencer::setLocks(scene, pattern, track, steps, static_cast<model::LockSlot>(slot), values,
                                         kNeverRemove, spec.label);
}
} // namespace core::perf
