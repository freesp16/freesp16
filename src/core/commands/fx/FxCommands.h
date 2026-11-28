// FX commands (state-and-undo.md; spec 02-features/effects/parameters.md FX-006/007/073…075/091…093, README.md
// FX-008/009): the edits of an FX slot that are more than one SetParam. Message thread only; each is one undo step.
//
//   SelectFxType   save the slot's current settings into its typeMemory, then restore typeMemory[new type] or the
//                  effect's defaults for this slot kind (core::fx::registerEffect's defInsert/defSend/defMaster,
//                  FX-092/093). Selecting a type never resets a remembered type's parameters (FX-006). Off keeps
//                  the memory and zeroes the slot's own fields (FX-091).
//   ToggleFxSync   FX-073…075: converts every time/rate parameter of the current effect (core::fx::convertSync at
//                  the tempo of the press) and flips `sync`, in one command.
//   makeSetFxBypass / makeSetFxMode   SetParam<bool> / SetParam<FxMode> (FX-045/046, FX-008/009; no coalescing).
//
// A slot is addressed by a model::ParamPath with scope InsertFx (scene, track, slot = FX1 0 / FX2 1), SendFx or
// MasterFx (scene). The slot's params[] are indexed by the internal index k (FX-060). Undo restores the whole slot
// (a small value snapshot: type, enums, six values and the type memory — EDT-U1).
#pragma once

#include "core/commands/Command.h"
#include "core/model/EffectSlot.h"
#include "core/model/paths/ParamPath.h"

#include <memory>
#include <string>
#include <string_view>

namespace core
{
model::EffectSlot& fxSlotAt(model::Project&, const model::ParamPath&);
const model::EffectSlot& fxSlotAt(const model::Project&, const model::ParamPath&);
std::string fxSlotPath(const model::ParamPath&); // "scene[s].track[t].insertFx[u]", "scene[s].sendFx", "…masterFx"
std::uint8_t fxSlotBit(const model::ParamPath&) noexcept; // core::fx::kSlotInsert / kSlotSend / kSlotMaster
bool fxTypeAllowed(const model::ParamPath&, model::EffectType) noexcept;

// Restores a slot snapshot (the inverse of every FX command).
class RestoreFxSlot final : public Command
{
public:
    RestoreFxSlot(model::ParamPath at, model::EffectSlot slot, std::string_view label)
        : at_(at), slot_(std::move(slot)), label_(label)
    {
    }
    std::string_view label() const override { return label_; }
    void apply(model::Project& p) override { fxSlotAt(p, at_) = slot_; }
    std::unique_ptr<Command> inverse(const model::Project& before) const override;
    void collectChanges(ChangeSet& c) const override { c.add(fxSlotPath(at_)); }
    std::size_t sizeBytes() const override;

private:
    model::ParamPath at_;
    model::EffectSlot slot_;
    std::string_view label_;
};

class SelectFxType final : public Command
{
public:
    SelectFxType(model::ParamPath at, model::EffectType type, model::PluginTrack plugin = {})
        : at_(at), type_(type), plugin_(std::move(plugin))
    {
    }
    std::string_view label() const override { return "FX TYPE"; }
    void apply(model::Project&) override; // a type the slot cannot hold (FX-040) is ignored
    std::unique_ptr<Command> inverse(const model::Project& before) const override;
    void collectChanges(ChangeSet& c) const override { c.add(fxSlotPath(at_)); }

private:
    model::ParamPath at_;
    model::EffectType type_;
    model::PluginTrack plugin_;
};

class ToggleFxSync final : public Command
{
public:
    ToggleFxSync(model::ParamPath at, double bpm) : at_(at), bpm_(bpm) {}
    std::string_view label() const override { return "SYNC"; }
    void apply(model::Project&) override; // no-op for an effect without a SYNC family parameter
    std::unique_ptr<Command> inverse(const model::Project& before) const override;
    void collectChanges(ChangeSet& c) const override { c.add(fxSlotPath(at_)); }

private:
    model::ParamPath at_;
    double bpm_;
};

// Applies the effect defaults of `type` for the slot kind to `slot` (FX-092): params by k, sync/lookAhead off,
// type1/type2 from the options "type1"/"type2" when the effect registered them, else 0.
void applyFxDefaults(model::EffectSlot& slot, model::EffectType type, std::uint8_t slotBit);

std::unique_ptr<Command> makeSetFxBypass(const model::ParamPath& slot, bool on); // any slot (FX-045/046)
std::unique_ptr<Command> makeSetFxMode(std::uint8_t scene, model::FxMode mode);
} // namespace core
