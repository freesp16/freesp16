// SetParam<T> (state-and-undo.md "Commands", family `SetParam`): sets one value of the document model — track volume,
// an FX parameter, an envelope attack, a step cell, the tempo. Its inverse is another SetParam holding the value
// before (a small value command, never a model snapshot; EDT-U1), and consecutive SetParams on the same path coalesce
// into one undo step (EDT-U2; the UndoStack applies the 1 s window).
//
// Where the value lives is a captureless accessor plus the indices it reads (a model::ParamPath), e.g.
//
//     constexpr auto trackVolume = [](model::Project& p, const model::ParamPath& at) -> int& {
//         return p.scenes[at.scene].tracks[at.track].volume; };
//     bus.execute(makeSetParam<int>("scene[0].track[3].volume", "VOLUME", trackVolume, {Scope::Track, 0, 3}, 90));
//
// so applying is one indirect call and a store — no path parsing on the message thread's hot path. The path string
// is the model path (data-model.md "Identity and paths") reported to observers and the EngineBridge, and the
// coalescing key; `label` is the undo label (ParamSpec::label where it applies) and must outlive the command
// (string literals, registry storage).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/model/paths/ParamPath.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace core
{
template <typename T> class SetParam final : public Command
{
public:
    // Returns the model field `at` addresses. It only forms the reference: inverse() reads through it from a const
    // project (const_cast, never written).
    using Accessor = T& (*)(model::Project&, const model::ParamPath& at);
    enum class Coalesce : bool
    {
        No = false,
        Yes = true
    };

    SetParam(std::string path, std::string_view label, Accessor field, model::ParamPath at, T value,
             Coalesce coalesce = Coalesce::Yes)
        : path_(std::move(path)), label_(label), field_(field), at_(at), value_(std::move(value)), coalesce_(coalesce)
    {
    }

    const std::string& path() const noexcept { return path_; }
    const T& value() const noexcept { return value_; }
    const model::ParamPath& at() const noexcept { return at_; }

    std::string_view label() const override { return label_; }
    void apply(model::Project& project) override { field_(project, at_) = value_; }
    std::unique_ptr<Command> inverse(const model::Project& before) const override
    {
        const T& old = field_(const_cast<model::Project&>(before), at_);
        return std::make_unique<SetParam>(path_, label_, field_, at_, old, coalesce_);
    }
    // `other` is a newer edit's inverse that would run before this one: a SetParam of the same type on the same path
    // is absorbed (this one already restores the older value). The time window is the UndoStack's.
    bool mergeWith(const Command& other, double) override
    {
        if (coalesce_ == Coalesce::No)
            return false;
        const auto* o = dynamic_cast<const SetParam*>(&other);
        return o != nullptr && o->coalesce_ == Coalesce::Yes && o->field_ == field_ && o->path_ == path_;
    }
    void collectChanges(ChangeSet& changes) const override { changes.add(path_); }
    std::size_t sizeBytes() const override { return sizeof(*this) + heapBytes(path_) + heapBytes(value_); }

private:
    template <typename U> static std::size_t heapBytes(const U& v) noexcept
    {
        if constexpr (std::is_same_v<U, std::string>)
            return v.capacity() > sizeof(std::string) ? v.capacity() : 0; // past the small-string buffer
        else
            return 0;
    }

    std::string path_;
    std::string_view label_;
    Accessor field_;
    model::ParamPath at_;
    T value_;
    Coalesce coalesce_;
};

template <typename T>
std::unique_ptr<Command> makeSetParam(std::string path, std::string_view label, typename SetParam<T>::Accessor field,
                                      model::ParamPath at, T value,
                                      typename SetParam<T>::Coalesce coalesce = SetParam<T>::Coalesce::Yes)
{
    return std::make_unique<SetParam<T>>(std::move(path), label, field, at, std::move(value), coalesce);
}
} // namespace core
