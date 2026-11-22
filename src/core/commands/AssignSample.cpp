#include "core/commands/AssignSample.h"

#include "core/model/Project.h"

#include <algorithm>
#include <utility>

namespace core
{
namespace
{
std::size_t indexOf(const model::SamplePool& pool, const std::string& id) noexcept
{
    for (std::size_t i = 0; i < pool.entries.size(); ++i)
        if (pool.entries[i].id == id)
            return i;
    return SIZE_MAX;
}
} // namespace

AssignSample::AssignSample(std::uint8_t scene, std::uint8_t track, model::SampleRef ref)
    : scene_(scene), track_(track), key_{ref.id}
{
    if (! ref.id.empty())
        addEntry_ = std::move(ref);
}

void AssignSample::apply(model::Project& project)
{
    auto& pool = project.sampleRefs;
    if (addEntry_ && indexOf(pool, addEntry_->id) == SIZE_MAX)
    {
        const std::size_t at = std::min(addAt_, pool.entries.size());
        pool.entries.insert(pool.entries.begin() + static_cast<std::ptrdiff_t>(at), *addEntry_);
    }
    project.scenes[scene_].tracks[track_].playback.sample = key_;
    if (! removeEntry_.empty())
    {
        const std::size_t at = indexOf(pool, removeEntry_);
        if (at != SIZE_MAX)
            pool.entries.erase(pool.entries.begin() + static_cast<std::ptrdiff_t>(at));
    }
}

std::unique_ptr<Command> AssignSample::inverse(const model::Project& before) const
{
    std::unique_ptr<AssignSample> inv(
        new AssignSample(scene_, track_, before.scenes[scene_].tracks[track_].playback.sample));
    if (addEntry_ && indexOf(before.sampleRefs, addEntry_->id) == SIZE_MAX)
        inv->removeEntry_ = addEntry_->id; // this assignment adds the record: its undo removes it (never the file)
    if (! removeEntry_.empty())
    {
        const std::size_t at = indexOf(before.sampleRefs, removeEntry_);
        if (at != SIZE_MAX)
        {
            inv->addEntry_ = before.sampleRefs.entries[at];
            inv->addAt_ = at;
        }
    }
    return inv;
}

void AssignSample::collectChanges(ChangeSet& changes) const
{
    changes.add("scene[" + std::to_string(scene_) + "].track[" + std::to_string(track_) + "].playback.sample");
    if (addEntry_ || ! removeEntry_.empty())
        changes.add("sampleRefs");
}

std::size_t AssignSample::sizeBytes() const
{
    std::size_t bytes = sizeof(*this) + key_.id.capacity() + removeEntry_.capacity();
    if (addEntry_)
        bytes += addEntry_->id.capacity() + addEntry_->path.capacity() + addEntry_->displayName.capacity();
    return bytes;
}
} // namespace core
