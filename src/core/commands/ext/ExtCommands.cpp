#include "core/commands/ext/ExtCommands.h"

#include "core/model/Project.h"

#include <algorithm>

namespace core::commands::ext
{
namespace
{
model::Extras& extrasOf(model::Project& p, int scene, int track)
{
    if (scene < 0 || track < 0)
        return p.extras;
    return p.scenes[static_cast<std::size_t>(scene & 15)].tracks[static_cast<std::size_t>(track & 15)].extras;
}
const model::Extras& extrasOf(const model::Project& p, int scene, int track)
{
    return extrasOf(const_cast<model::Project&>(p), scene, track);
}

class SetExtra final : public Command
{
public:
    SetExtra(int scene, int track, std::string key, std::optional<std::string> value, std::string label)
        : scene_(scene), track_(track), key_(std::move(key)), value_(std::move(value)), label_(std::move(label))
    {
    }
    std::string_view label() const override { return label_; }
    void apply(model::Project& p) override
    {
        auto& e = extrasOf(p, scene_, track_);
        if (value_)
            e.set(key_, *value_, model::Extras::Encoding::Json);
        else
            e.entries.erase(std::remove_if(e.entries.begin(), e.entries.end(),
                                           [&](const model::Extras::Entry& x) { return x.key == key_; }),
                            e.entries.end());
    }
    std::unique_ptr<Command> inverse(const model::Project& before) const override
    {
        const auto* old = extrasOf(before, scene_, track_).find(key_);
        return std::make_unique<SetExtra>(scene_, track_, key_,
                                          old != nullptr ? std::optional<std::string>(old->value) : std::nullopt, label_);
    }
    void collectChanges(ChangeSet& c) const override { c.add(extraPath(scene_, track_, key_)); }
    std::size_t sizeBytes() const override { return sizeof(*this) + key_.size() + (value_ ? value_->size() : 0); }

private:
    int scene_, track_;
    std::string key_;
    std::optional<std::string> value_;
    std::string label_;
};
} // namespace

std::unique_ptr<Command> setExtra(int scene, int track, std::string key, std::optional<std::string> value,
                                  std::string_view label)
{
    return std::make_unique<SetExtra>(scene, track, std::move(key), std::move(value), std::string(label));
}

std::string extraPath(int scene, int track, std::string_view key)
{
    if (scene < 0 || track < 0)
        return "extras." + std::string(key);
    return "scene[" + std::to_string(scene & 15) + "].track[" + std::to_string(track & 15) + "].extras." +
           std::string(key);
}
} // namespace core::commands::ext
