// AssignSample (state-and-undo.md family `AssignSample`): loads a sample into a track or changes it — the track's
// SampleKey (playback.sample) and, when the pool does not know the sample yet, its pool entry (data-model.md
// "SamplePool": every sample used by any track, once).
//
// EDT-U5: undo restores the reference only. A command never touches files: the inverse puts the previous key back
// and removes the pool entry this assignment added (the record of the file, not the file — the audio stays on disk
// and in the library, so redo just adds the record again). Inverses hold keys and at most one SampleRef (EDT-U1).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Sample.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace core
{
class AssignSample final : public Command
{
public:
    // Assign `ref` (its id is the key) to scene[scene].track[track]; adds `ref` to the pool when its id is new. An
    // empty ref.id clears the assignment.
    AssignSample(std::uint8_t scene, std::uint8_t track, model::SampleRef ref);

    std::string_view label() const override { return "SAMPLE"; }
    void apply(model::Project&) override;
    std::unique_ptr<Command> inverse(const model::Project& before) const override;
    void collectChanges(ChangeSet&) const override;
    std::size_t sizeBytes() const override;

    const model::SampleKey& key() const noexcept { return key_; }

private:
    AssignSample(std::uint8_t scene, std::uint8_t track, model::SampleKey key)
        : scene_(scene), track_(track), key_(std::move(key))
    {
    }

    std::uint8_t scene_, track_;
    model::SampleKey key_;                     // the track's reference after apply
    std::optional<model::SampleRef> addEntry_; // pool record to add if its id is absent …
    std::size_t addAt_ = SIZE_MAX;             // … at this index (SIZE_MAX = the end)
    std::string removeEntry_;                  // pool record to remove (an inverse of an add); "" = none
};
} // namespace core
