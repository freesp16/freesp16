#include "engine/modulation/ModMatrix.h"

#include "engine/fx/FxSlotBank.h"

#include <utility>

namespace engine::modulation
{
namespace
{
constexpr bool validTrack(int track, int n) noexcept { return track >= 0 && track < n; }
} // namespace

void ModMatrix::prepare(int tracks, int maxDestsPerTrack) noexcept
{
    numTracks_ = tracks < 1 ? 1 : (tracks > kModTracks ? kModTracks : tracks);
    numDests_ = maxDestsPerTrack < 1 ? 1 : (maxDestsPerTrack > kModDestsPerTrack ? kModDestsPerTrack : maxDestsPerTrack);
    tracks_ = {};
}

ModMatrix::Slot* ModMatrix::find(int track, core::ParamId dest) noexcept
{
    auto& t = tracks_[static_cast<std::size_t>(track)];
    for (int i = 0; i < numDests_; ++i)
        if (((t.used >> i) & 1u) != 0 && t.slot[static_cast<std::size_t>(i)].dest == dest)
            return &t.slot[static_cast<std::size_t>(i)];
    return nullptr;
}

const ModMatrix::Slot* ModMatrix::find(int track, core::ParamId dest) const noexcept
{
    return const_cast<ModMatrix*>(this)->find(track, dest);
}

bool ModMatrix::set(int track, core::ParamId dest, ModSource src, ModKind kind, float value01) noexcept
{
    const int s = static_cast<int>(src);
    if (! validTrack(track, numTracks_) || ! dest.valid() || s < 0 || s >= kModSources)
        return false;
    if (! (value01 == value01)) // NaN never reaches a destination
        value01 = 0.0f;
    Slot* slot = find(track, dest);
    if (slot == nullptr)
    {
        auto& t = tracks_[static_cast<std::size_t>(track)];
        for (int i = 0; i < numDests_ && slot == nullptr; ++i)
            if (((t.used >> i) & 1u) == 0)
            {
                slot = &t.slot[static_cast<std::size_t>(i)];
                *slot = Slot {};
                slot->dest = dest;
                t.used = static_cast<std::uint8_t>(t.used | (1u << i));
            }
        if (slot == nullptr)
            return false;
    }
    const auto bit = static_cast<std::uint8_t>(1u << s);
    const auto overrides = kind == ModKind::Override ? static_cast<std::uint8_t>(slot->overrides | bit)
                                                     : static_cast<std::uint8_t>(slot->overrides & ~bit);
    if ((slot->sources & bit) == 0 || slot->value[s] != value01 || slot->overrides != overrides)
        slot->dirty = true;
    slot->sources = static_cast<std::uint8_t>(slot->sources | bit);
    slot->overrides = overrides;
    slot->value[s] = value01;
    return true;
}

void ModMatrix::clear(int track, ModSource src) noexcept
{
    const int s = static_cast<int>(src);
    if (! validTrack(track, numTracks_) || s < 0 || s >= kModSources)
        return;
    auto& t = tracks_[static_cast<std::size_t>(track)];
    const auto bit = static_cast<std::uint8_t>(1u << s);
    for (int i = 0; i < numDests_; ++i)
    {
        auto& slot = t.slot[static_cast<std::size_t>(i)];
        if (((t.used >> i) & 1u) != 0 && (slot.sources & bit) != 0)
        {
            slot.sources = static_cast<std::uint8_t>(slot.sources & ~bit);
            slot.overrides = static_cast<std::uint8_t>(slot.overrides & ~bit);
            slot.value[s] = 0.0f;
            slot.dirty = true;
        }
    }
}

void ModMatrix::clear(int track, core::ParamId dest, ModSource src) noexcept
{
    const int s = static_cast<int>(src);
    if (! validTrack(track, numTracks_) || s < 0 || s >= kModSources)
        return;
    if (Slot* slot = find(track, dest); slot != nullptr && (slot->sources & (1u << s)) != 0)
    {
        slot->sources = static_cast<std::uint8_t>(slot->sources & ~(1u << s));
        slot->overrides = static_cast<std::uint8_t>(slot->overrides & ~(1u << s));
        slot->value[s] = 0.0f;
        slot->dirty = true;
    }
}

void ModMatrix::clearAll(ModSource src) noexcept
{
    for (int t = 0; t < numTracks_; ++t)
        clear(t, src);
}

void ModMatrix::touch(int track) noexcept
{
    if (! validTrack(track, numTracks_))
        return;
    auto& t = tracks_[static_cast<std::size_t>(track)];
    for (int i = 0; i < numDests_; ++i)
        if (((t.used >> i) & 1u) != 0)
            t.slot[static_cast<std::size_t>(i)].dirty = true;
}

void ModMatrix::touchAll() noexcept
{
    for (int t = 0; t < numTracks_; ++t)
        touch(t);
}

void ModMatrix::swapTracks(int a, int b) noexcept
{
    std::swap(tracks_[a], tracks_[b]);
    touch(a);
    touch(b);
}

ModValue ModMatrix::sum(const Slot& slot) noexcept
{
    ModValue v;
    for (int s = 0; s < kModSources; ++s)
    {
        if (((slot.sources >> s) & 1u) == 0)
            continue;
        if (((slot.overrides >> s) & 1u) != 0)
        {
            v.override01 = slot.value[s]; // the highest source wins
            v.hasOverride = true;
        }
        else
            v.offset01 += slot.value[s];
    }
    return v;
}

// Per dirty slot: FX insert knob → FxSlotBank::setModulation (the bank clamps effective + amount), else the sink.
// A slot whose sources are all gone is released once and freed.
void ModMatrix::applyBlock(fx::FxSlotBank& fx, ParamStateSink& sink) noexcept
{
    const auto& routes = fx.routes();
    for (int track = 0; track < numTracks_; ++track)
    {
        auto& t = tracks_[static_cast<std::size_t>(track)];
        for (std::uint32_t used = t.used, i = 0; used != 0; used >>= 1, ++i)
        {
            auto& slot = t.slot[i];
            if ((used & 1u) == 0 || ! slot.dirty)
                continue;
            slot.dirty = false;
            const bool empty = slot.sources == 0;
            const fx::FxRoute& r = routes.find(slot.dest);
            if (r.kind == fx::FxRoute::Kind::Knob && r.slotKind == fx::SlotKind::Insert)
            {
                const int s = fx::insertSlot(track, r.unit);
                const bool present = fx.slot(s).pending() == r.type; // FX-085: another type changes nothing
                slot.stale = ! present;
                if (empty)
                {
                    // Zero the bank's amount even for an absent type (no audible change: the bank pushes only the
                    // slot's current type), so no stale amount waits for the type's return. The bank modulates one
                    // type per slot: switching it drops another destination's amount, so the track's others are
                    // re-applied (at the latest in the next block).
                    if (slot.applied)
                    {
                        fx.setModulation(track, r.unit, r.type, r.index, 0.0f);
                        if (! present)
                            touch(track);
                    }
                }
                else if (present)
                {
                    const ModValue v = sum(slot);
                    // Override: the amount moves the effective value to the override (the bank clamps the sum).
                    const float amount = v.hasOverride ? v.override01 - fx.effective(s, r.index) + v.offset01 : v.offset01;
                    fx.setModulation(track, r.unit, r.type, r.index, amount);
                    slot.applied = true;
                    // An Override follows the effective value (locks at triggers): re-applied every block.
                    slot.dirty = v.hasOverride;
                }
                else
                    slot.dirty = true; // re-checked every block until the type returns (then applied at once)
            }
            else if (empty)
            {
                if (slot.applied)
                    sink.release(track, slot.dest);
            }
            else
                slot.applied = sink.modulate(track, slot.dest, sum(slot)) || slot.applied;
            if (empty)
                t.used = static_cast<std::uint8_t>(t.used & ~(1u << i));
        }
    }
}

ModValue ModMatrix::value(int track, core::ParamId dest) const noexcept
{
    if (! validTrack(track, numTracks_))
        return {};
    const Slot* slot = find(track, dest);
    return slot != nullptr ? sum(*slot) : ModValue {};
}

float ModMatrix::contribution(int track, core::ParamId dest, ModSource src) const noexcept
{
    const int s = static_cast<int>(src);
    if (! validTrack(track, numTracks_) || s < 0 || s >= kModSources)
        return 0.0f;
    const Slot* slot = find(track, dest);
    return slot != nullptr && ((slot->sources >> s) & 1u) != 0 ? slot->value[s] : 0.0f;
}

bool ModMatrix::modulated(int track, core::ParamId dest) const noexcept
{
    if (! validTrack(track, numTracks_))
        return false;
    const Slot* slot = find(track, dest);
    return slot != nullptr && slot->sources != 0;
}

int ModMatrix::activeDestinations(int track) const noexcept
{
    if (! validTrack(track, numTracks_))
        return 0;
    const auto& t = tracks_[static_cast<std::size_t>(track)];
    int n = 0;
    for (int i = 0; i < numDests_; ++i)
        if (((t.used >> i) & 1u) != 0 && t.slot[static_cast<std::size_t>(i)].sources != 0)
            ++n;
    return n;
}

bool ModMatrix::stale(int track, core::ParamId dest) const noexcept
{
    if (! validTrack(track, numTracks_))
        return false;
    const Slot* slot = find(track, dest);
    return slot != nullptr && slot->stale;
}
} // namespace engine::modulation
