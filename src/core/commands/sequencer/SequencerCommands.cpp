#include "core/commands/sequencer/SequencerCommands.h"

#include "core/model/Project.h"
#include "core/params/catalogue/fx.h"
#include "core/util/sequencer/RecordQuantize.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <vector>

namespace core::commands::sequencer
{
namespace
{
using model::LockSlot;
using model::Pattern;
using model::Step;
using model::StepLocks;
namespace cs = ::core::sequencer;

constexpr int kSlotOffset = cs::kLockSlotOffset;

bool validStep(int s) noexcept { return s >= 0 && s < model::kSteps; }
bool validSlot(int s) noexcept { return s >= 0 && s < model::kLockSlots; }

Pattern& patternOf(model::Project& p, int scene, int pattern) noexcept
{
    return p.scenes[static_cast<std::size_t>(scene & 15)].patterns[static_cast<std::size_t>(pattern & 15)];
}
Step& cellOf(Pattern& p, int step, int track) noexcept
{
    return p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track & 15)];
}

// Drops lock entries no cell references or whose mask is empty; keeps the order of the others.
void compactLocks(Pattern& p)
{
    const std::size_t n = p.locks.size();
    if (n == 0)
        return;
    std::vector<int> refs(n, 0);
    for (auto& row : p.steps)
        for (auto& c : row)
        {
            if (c.lockIndex == Step::kNoLocks)
                continue;
            if (c.lockIndex >= n || p.locks[c.lockIndex].mask == 0)
                c.lockIndex = Step::kNoLocks;
            else
                ++refs[c.lockIndex];
        }
    std::vector<std::uint16_t> remap(n, Step::kNoLocks);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (refs[i] > 0)
        {
            remap[i] = static_cast<std::uint16_t>(kept);
            if (kept != i)
                p.locks[kept] = p.locks[i];
            ++kept;
        }
    if (kept == n)
        return;
    p.locks.resize(kept);
    for (auto& row : p.steps)
        for (auto& c : row)
            if (c.lockIndex != Step::kNoLocks)
                c.lockIndex = remap[c.lockIndex];
}

StepLocks* locksOf(Pattern& p, Step& c) noexcept
{
    return c.lockIndex != Step::kNoLocks && c.lockIndex < p.locks.size() ? &p.locks[c.lockIndex] : nullptr;
}

void putLock(Pattern& p, Step& c, LockSlot slot, float value)
{
    StepLocks* l = locksOf(p, c);
    if (l == nullptr)
    {
        p.locks.emplace_back();
        c.lockIndex = static_cast<std::uint16_t>(p.locks.size() - 1);
        l = &p.locks.back();
    }
    l->mask |= std::uint64_t {1} << slot;
    l->values[slot] = value;
}

void dropLock(Pattern& p, Step& c, LockSlot slot) noexcept
{
    if (StepLocks* l = locksOf(p, c))
        l->mask &= ~(std::uint64_t {1} << slot);
}

std::int16_t codeOf(model::TriggerType t) noexcept
{
    return t == model::TriggerType::Half       ? cs::kTriggerCodeHalf
           : t == model::TriggerType::LockOnly ? cs::kTriggerCodeLockOnly
                                               : cs::kTriggerCodeFull;
}

void setTrigger(Step& c, model::TriggerType type, int velocity) noexcept
{
    c.trigger = codeOf(type);
    c.triggerType = type;
    c.value2 = static_cast<std::int16_t>(std::clamp(velocity, 1, 127));
}

void clearCell(Step& c) noexcept
{
    c.trigger = Step::kEmpty;
    c.value2 = Step::kNoValue2;
    c.triggerType = model::TriggerType::Full;
    c.lockIndex = Step::kNoLocks;
}

// The arguments every edit uses; `fn` interprets them.
struct EditArgs
{
    int track = 0;
    std::vector<int> steps;     // the cells it touches
    std::vector<float> values;  // per step (setLocks) or [0] (setLock)
    int a = 0, b = 0;           // velocity / offset / count / detents
    float f = 0.0f;             // strength / removeAt
    LockSlot slot = 0;
    model::TriggerType type = model::TriggerType::Full;
};
using EditFn = void (*)(Pattern&, const EditArgs&);

// Restores one pattern (the inverse of every edit; also the redo of an undo). Merge key 0 = never coalesces.
class PatternRestore final : public core::Command
{
public:
    PatternRestore(int scene, int pattern, std::unique_ptr<Pattern> value, std::string_view label, std::uint64_t key)
        : scene_(scene), pattern_(pattern), value_(std::move(value)), label_(label), key_(key)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(model::Project& p) override { patternOf(p, scene_, pattern_) = *value_; }
    std::unique_ptr<core::Command> inverse(const model::Project& before) const override
    {
        return std::make_unique<PatternRestore>(
            scene_, pattern_, std::make_unique<Pattern>(patternOf(const_cast<model::Project&>(before), scene_, pattern_)),
            label_, key_);
    }
    // `other` is a newer edit's inverse that would run before this one: this one already restores the older pattern.
    bool mergeWith(const core::Command& other, double) override
    {
        const auto* o = dynamic_cast<const PatternRestore*>(&other);
        return key_ != 0 && o != nullptr && o->key_ == key_ && o->scene_ == scene_ && o->pattern_ == pattern_;
    }
    void collectChanges(core::ChangeSet& c) const override { c.add(patternPath(scene_, pattern_)); }
    std::size_t sizeBytes() const override
    {
        return sizeof(*this) + sizeof(Pattern) + value_->locks.capacity() * sizeof(StepLocks);
    }

private:
    int scene_, pattern_;
    std::unique_ptr<Pattern> value_;
    std::string_view label_;
    std::uint64_t key_;
};

class PatternEdit final : public core::Command
{
public:
    PatternEdit(int scene, int pattern, std::string_view label, EditFn fn, EditArgs args, std::uint64_t key)
        : scene_(scene & 15), pattern_(pattern & 15), label_(label), fn_(fn), args_(std::move(args)), key_(key)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(model::Project& p) override
    {
        Pattern& pat = patternOf(p, scene_, pattern_);
        fn_(pat, args_);
        compactLocks(pat);
    }
    std::unique_ptr<core::Command> inverse(const model::Project& before) const override
    {
        return std::make_unique<PatternRestore>(
            scene_, pattern_, std::make_unique<Pattern>(patternOf(const_cast<model::Project&>(before), scene_, pattern_)),
            label_, key_);
    }
    void collectChanges(core::ChangeSet& c) const override { c.add(patternPath(scene_, pattern_)); }
    std::size_t sizeBytes() const override
    {
        return sizeof(*this) + args_.steps.capacity() * sizeof(int) + args_.values.capacity() * sizeof(float);
    }

private:
    int scene_, pattern_;
    std::string_view label_;
    EditFn fn_;
    EditArgs args_;
    std::uint64_t key_;
};

// Merge keys: kind in the top byte; 0 = no coalescing.
constexpr std::uint64_t kKeyRecord = 1ull << 56;                // every recordHit / erase of a pattern (one pass)
std::uint64_t lockKey(int track, int step, int slot) noexcept // hold + turn on one cell and slot
{
    return 2ull << 56 | static_cast<std::uint64_t>(slot & 0xFF) << 16 | static_cast<std::uint64_t>(track & 0xFF) << 8 |
           static_cast<std::uint64_t>(step & 0xFF);
}
std::uint64_t stripKey(int track, int slot) noexcept
{
    return 3ull << 56 | static_cast<std::uint64_t>(slot) << 8 | static_cast<std::uint64_t>(track & 0xFF);
}
std::uint64_t shiftKey(int track) noexcept { return 4ull << 56 | static_cast<std::uint64_t>(track & 0xFF); }
std::uint64_t motionKey(int track, int slot) noexcept
{
    return 5ull << 56 | static_cast<std::uint64_t>(slot & 0xFF) << 8 | static_cast<std::uint64_t>(track & 0xFF);
}

// A PARAM step whose last lock went is empty again (it would only cost a cell otherwise).
void dropEmptyLockOnly(Pattern& p, Step& c) noexcept
{
    if (c.triggerType != model::TriggerType::LockOnly || c.trigger == Step::kEmpty)
        return;
    const StepLocks* l = locksOf(p, c);
    if (l == nullptr || l->mask == 0)
        clearCell(c);
}

std::unique_ptr<core::Command> edit(int scene, int pattern, std::string_view label, EditFn fn, EditArgs args,
                                    std::uint64_t key = 0)
{
    return std::make_unique<PatternEdit>(scene, pattern, label, fn, std::move(args), key);
}

EditArgs one(int track, int step)
{
    EditArgs a;
    a.track = track & 15;
    a.steps.push_back(step);
    return a;
}

// ---- the lockable parameters (sequencer.md "Lockable parameters", ParamIndex order; OQ-01) ----
struct SlotId
{
    std::string_view id;
    int slot;
};
constexpr SlotId kSlotIds[] = {
    {"track.volume", 0},       {"playback.start", 1},     {"playback.loopStart", 2}, {"playback.length", 3},
    {"playback.pitch", 4},     {"track.pan", 5},          {"track.sendAmount", 6},   {"playback.timeStretch", 7},
    {"playback.loop", 8},      {"track.triggerMode", 9},  {"track.mute", 10},        {"env.velocity", 11},
    {"env.attack", 12},        {"env.hold", 13},          {"env.release", 14},       {"seq.offset", 15},
    {"seq.retrigCount", 16},   {"seq.retrigSpeed", 17},   {"lfo.speed", 18},         {"lfo.depth", 19},
    {"lfo.phase", 20},         {"playback.reverse", 21},  {"slice.number", 22},
};
} // namespace

std::string patternPath(int scene, int pattern)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "scene[%d].pattern[%d]", scene & 15, pattern & 15);
    return buf;
}

bool parsePatternPath(std::string_view path, int& scene, int& pattern) noexcept
{
    constexpr std::string_view kScene = "scene[", kPattern = "].pattern[";
    if (path.substr(0, kScene.size()) != kScene)
        return false;
    const char* p = path.data() + kScene.size();
    const char* end = path.data() + path.size();
    int s = 0, q = 0;
    auto r = std::from_chars(p, end, s);
    if (r.ec != std::errc {} || static_cast<std::size_t>(end - r.ptr) < kPattern.size() ||
        std::string_view(r.ptr, kPattern.size()) != kPattern)
        return false;
    r = std::from_chars(r.ptr + kPattern.size(), end, q);
    if (r.ec != std::errc {} || r.ptr == end || *r.ptr != ']' || s < 0 || s > 15 || q < 0 || q > 15)
        return false;
    scene = s;
    pattern = q;
    return true;
}

int lockSlotOf(const core::ParamSpec& spec, const core::ParamRegistry& registry)
{
    if (! spec.lockable)
        return -1;
    for (const auto& s : kSlotIds)
        if (s.id == spec.id)
            return s.slot;
    const bool fx1 = spec.id.substr(0, 4) == "fx1.", fx2 = spec.id.substr(0, 4) == "fx2.";
    if (! fx1 && ! fx2)
        return -1;
    const auto id = registry.idOf(spec.id);
    if (! id)
        return -1;
    static const std::vector<std::int8_t> builtin = core::fx::internalIndexTable(core::builtinParams());
    const std::vector<std::int8_t> own = &registry == &core::builtinParams() ? std::vector<std::int8_t> {}
                                                                              : core::fx::internalIndexTable(registry);
    const auto& internal = &registry == &core::builtinParams() ? builtin : own;
    if (id->value >= internal.size())
        return -1;
    const int k = internal[id->value];
    return k < 0 || k > 5 ? -1 : (fx1 ? 23 : 29) + k;
}

bool lockOf(const model::Pattern& p, int step, int track, model::LockSlot slot, float& value) noexcept
{
    if (! validStep(step) || ! validSlot(slot))
        return false;
    const auto& c = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track & 15)];
    if (c.lockIndex == Step::kNoLocks || c.lockIndex >= p.locks.size())
        return false;
    const auto& l = p.locks[c.lockIndex];
    if ((l.mask >> slot & 1u) == 0)
        return false;
    value = l.values[slot];
    return true;
}

void makePatternImage(const model::Pattern& p, cs::PatternImage& out)
{
    out.length = static_cast<std::uint8_t>(std::clamp(p.length, 1, 64));
    out.trackMutes = out.trackSolos = 0;
    for (int t = 0; t < model::kTracks; ++t)
    {
        if (p.trackMutes[static_cast<std::size_t>(t)])
            out.trackMutes = static_cast<std::uint16_t>(out.trackMutes | 1u << t);
        if (p.trackSolos[static_cast<std::size_t>(t)])
            out.trackSolos = static_cast<std::uint16_t>(out.trackSolos | 1u << t);
    }
    out.locks.clear();
    out.locks.reserve(p.locks.size());
    for (const auto& l : p.locks)
    {
        cs::ImageLocks il;
        il.mask = l.mask & ((std::uint64_t {1} << model::kLockSlots) - 1);
        for (int s = 0; s < model::kLockSlots; ++s)
        {
            il.values[static_cast<std::size_t>(s)] = l.values[static_cast<std::size_t>(s)];
            il.change[static_cast<std::size_t>(s)] = static_cast<std::uint8_t>(l.change[static_cast<std::size_t>(s)]);
        }
        out.locks.push_back(il);
    }
    for (int s = 0; s < model::kSteps; ++s)
        for (int t = 0; t < model::kTracks; ++t)
        {
            const auto& c = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            auto& o = out.cells[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            o = {};
            if (c.trigger != Step::kEmpty)
            {
                o.velocity = static_cast<std::uint8_t>(c.value2 >= 1 && c.value2 <= 127 ? c.value2 : 127);
                o.kind = static_cast<std::uint8_t>(c.triggerType);
            }
            if (c.lockIndex == Step::kNoLocks || c.lockIndex >= p.locks.size())
                continue;
            o.lockIndex = c.lockIndex;
            const auto& l = p.locks[c.lockIndex];
            if (l.mask >> kSlotOffset & 1u)
                o.offset24 = static_cast<std::int8_t>(
                    std::clamp(static_cast<int>(std::lround(l.values[kSlotOffset])), -24, 24));
            if (l.mask >> cs::kLockSlotRetrigCount & 1u)
                o.retrigCount = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(std::lround(l.values[cs::kLockSlotRetrigCount])), 1, 16));
            if (l.mask >> cs::kLockSlotRetrigSpeed & 1u)
                o.retrigSpeed = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(std::lround(l.values[cs::kLockSlotRetrigSpeed])), 0, 254) + 1);
        }
}

// ---- commands ----
std::unique_ptr<core::Command> toggleStep(int scene, int pattern, int track, int step, model::TriggerType type,
                                          int velocity)
{
    auto a = one(track, step);
    a.type = type;
    a.a = velocity;
    return edit(scene, pattern, "STEP", [](Pattern& p, const EditArgs& x) {
        if (! validStep(x.steps[0]))
            return;
        Step& c = cellOf(p, x.steps[0], x.track);
        if (c.trigger == Step::kEmpty && c.lockIndex == Step::kNoLocks)
            setTrigger(c, x.type, x.a);
        else
            clearCell(c);
    }, std::move(a));
}

std::unique_ptr<core::Command> clearStep(int scene, int pattern, int track, int step)
{
    return edit(scene, pattern, "CLEAR STEP", [](Pattern& p, const EditArgs& x) {
        if (validStep(x.steps[0]))
            clearCell(cellOf(p, x.steps[0], x.track));
    }, one(track, step), kKeyRecord);
}

std::unique_ptr<core::Command> setLock(int scene, int pattern, int track, int step, model::LockSlot slot, float value)
{
    auto a = one(track, step);
    a.slot = slot;
    a.values.push_back(value);
    return edit(scene, pattern, "LOCK", [](Pattern& p, const EditArgs& x) {
        if (! validStep(x.steps[0]) || ! validSlot(x.slot))
            return;
        Step& c = cellOf(p, x.steps[0], x.track);
        if (c.trigger == Step::kEmpty)
            setTrigger(c, model::TriggerType::LockOnly, 127); // a lock on an empty step makes it a PARAM step
        putLock(p, c, x.slot, x.values[0]);
    }, std::move(a), lockKey(track, step, slot));
}

std::unique_ptr<core::Command> removeLock(int scene, int pattern, int track, int step, model::LockSlot slot)
{
    auto a = one(track, step);
    a.slot = slot;
    return edit(scene, pattern, "REMOVE LOCK", [](Pattern& p, const EditArgs& x) {
        if (validStep(x.steps[0]) && validSlot(x.slot))
            dropLock(p, cellOf(p, x.steps[0], x.track), x.slot);
    }, std::move(a));
}

std::unique_ptr<core::Command> recordHit(int scene, int pattern, int track, int step, int velocity, int offset24,
                                         int pitchSlot, float pitch)
{
    auto a = one(track, step);
    a.a = velocity;
    a.b = std::clamp(offset24, -24, 24);
    a.f = validSlot(pitchSlot) ? 1.0f : 0.0f; // pitched
    a.slot = static_cast<LockSlot>(validSlot(pitchSlot) ? pitchSlot : 0);
    a.values.push_back(pitch);
    return edit(scene, pattern, "RECORD", [](Pattern& p, const EditArgs& x) {
        if (! validStep(x.steps[0]))
            return;
        Step& c = cellOf(p, x.steps[0], x.track);
        setTrigger(c, model::TriggerType::Full, x.a); // overdub: a new hit replaces the step's trigger (SEQ-046)
        if (x.b != 0)
            putLock(p, c, kSlotOffset, static_cast<float>(x.b));
        else
            dropLock(p, c, kSlotOffset);
        if (x.f == 0.0f)
            return;
        if (std::isnan(x.values[0]))
            dropLock(p, c, x.slot);
        else
            putLock(p, c, x.slot, x.values[0]);
    }, std::move(a), kKeyRecord);
}

std::unique_ptr<core::Command> clearTriggers(int scene, int pattern, int track, std::span<const int> steps)
{
    EditArgs a;
    a.track = track & 15;
    a.steps.assign(steps.begin(), steps.end());
    return edit(scene, pattern, "CLEAR TRIGGERS", [](Pattern& p, const EditArgs& x) {
        for (const int s : x.steps)
            if (validStep(s))
                clearCell(cellOf(p, s, x.track));
    }, std::move(a));
}

std::unique_ptr<core::Command> quantizeOffsets(int scene, int pattern, int track, std::span<const int> steps,
                                               float strength)
{
    EditArgs a;
    a.track = track & 15;
    a.steps.assign(steps.begin(), steps.end());
    a.f = std::clamp(strength, 0.0f, 1.0f);
    return edit(scene, pattern, "QUANTIZE", [](Pattern& p, const EditArgs& x) {
        for (const int s : x.steps)
        {
            if (! validStep(s))
                continue;
            Step& c = cellOf(p, s, x.track);
            StepLocks* l = locksOf(p, c);
            if (l == nullptr || (l->mask >> kSlotOffset & 1u) == 0)
                continue;
            const long v = std::lround(l->values[kSlotOffset] * (1.0f - x.f));
            if (v == 0)
                dropLock(p, c, kSlotOffset);
            else
                l->values[kSlotOffset] = static_cast<float>(v);
        }
    }, std::move(a));
}

std::unique_ptr<core::Command> recordLocks(int scene, int pattern, int track, std::span<const int> steps, LockSlot slot,
                                           float value)
{
    EditArgs a;
    a.track = track & 15;
    a.steps.assign(steps.begin(), steps.end());
    a.values.push_back(value);
    a.slot = slot;
    return edit(scene, pattern, "RECORD", [](Pattern& p, const EditArgs& x) {
        if (! validSlot(x.slot))
            return;
        for (const int step : x.steps)
        {
            if (! validStep(step))
                continue;
            Step& c = cellOf(p, step, x.track);
            if (c.trigger == Step::kEmpty)
                setTrigger(c, model::TriggerType::LockOnly, 127); // as setLock: a lock on an empty step plays
            putLock(p, c, x.slot, x.values[0]);
        }
    }, std::move(a), kKeyRecord);
}

std::unique_ptr<core::Command> setLocks(int scene, int pattern, int track, std::span<const int> steps, LockSlot slot,
                                        std::span<const float> values, float removeAt, std::string_view label)
{
    EditArgs a;
    a.track = track & 15;
    const std::size_t n = std::min(steps.size(), values.size());
    a.steps.assign(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(n));
    a.values.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(n));
    a.slot = slot;
    a.f = removeAt;
    return edit(scene, pattern, label, [](Pattern& p, const EditArgs& x) {
        if (! validSlot(x.slot))
            return;
        for (std::size_t i = 0; i < x.steps.size(); ++i)
        {
            if (! validStep(x.steps[i]))
                continue;
            Step& c = cellOf(p, x.steps[i], x.track);
            if (x.values[i] == x.f)
                dropLock(p, c, x.slot);
            else
                putLock(p, c, x.slot, x.values[i]);
        }
    }, std::move(a), stripKey(track, slot));
}

std::unique_ptr<core::Command> editMotion(int scene, int pattern, int track, std::span<const int> steps, LockSlot slot,
                                          std::span<const float> values)
{
    EditArgs a;
    a.track = track & 15;
    const std::size_t n = std::min(steps.size(), values.size());
    a.steps.assign(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(n));
    a.values.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(n));
    a.slot = slot;
    return edit(scene, pattern, "MOTION", [](Pattern& p, const EditArgs& x) {
        if (! validSlot(x.slot))
            return;
        for (std::size_t i = 0; i < x.steps.size(); ++i)
        {
            if (! validStep(x.steps[i]))
                continue;
            Step& c = cellOf(p, x.steps[i], x.track);
            if (std::isnan(x.values[i]))
            {
                dropLock(p, c, x.slot);
                dropEmptyLockOnly(p, c);
                continue;
            }
            if (c.trigger == Step::kEmpty)
                setTrigger(c, model::TriggerType::LockOnly, 127);
            putLock(p, c, x.slot, x.values[i]);
        }
    }, std::move(a), motionKey(track, slot));
}

std::unique_ptr<core::Command> clearMotion(int scene, int pattern, int track, std::uint64_t slotMask)
{
    EditArgs a;
    a.track = track & 15;
    for (int s = 0; s < model::kLockSlots; ++s)
        if (slotMask >> s & 1u)
            a.steps.push_back(s); // here: the slots to clear (every step of the track is visited)
    return edit(scene, pattern, "CLEAR MOTION", [](Pattern& p, const EditArgs& x) {
        for (int step = 0; step < model::kSteps; ++step)
        {
            Step& c = cellOf(p, step, x.track);
            if (c.lockIndex == Step::kNoLocks)
                continue;
            for (const int slot : x.steps)
                dropLock(p, c, static_cast<LockSlot>(slot));
            dropEmptyLockOnly(p, c);
        }
    }, std::move(a));
}

std::unique_ptr<core::Command> shiftSteps(int scene, int pattern, int track, int first, int count, int detents)
{
    EditArgs a;
    a.track = track & 15;
    a.a = std::clamp(first, 0, model::kSteps - 1);
    a.b = std::clamp(count, 1, model::kSteps - a.a);
    a.f = static_cast<float>(detents);
    return edit(scene, pattern, "SHIFT", [](Pattern& p, const EditArgs& x) {
        std::array<Step, model::kSteps> column; // default member initialisers, no {} (GCC 11)
        for (int i = 0; i < x.b; ++i)
            column[static_cast<std::size_t>(i)] = cellOf(p, x.a + i, x.track);
        const int d = static_cast<int>(x.f);
        const int r = ((d % x.b) + x.b) % x.b;
        for (int i = 0; i < x.b; ++i)
            cellOf(p, x.a + (i + r) % x.b, x.track) = column[static_cast<std::size_t>(i)];
    }, std::move(a), shiftKey(track));
}
} // namespace core::commands::sequencer
