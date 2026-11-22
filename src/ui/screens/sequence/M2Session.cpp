#include "ui/screens/sequence/M2Session.h"

#include "core/commands/CommandBusAccess.h"
#include "core/model/ModelOps.h"
#include "core/params/ParamRegistry.h"

#include <algorithm>
#include <cmath>

namespace ui::m2
{
Session& Session::instance() noexcept
{
    static Session session;
    return session;
}

void Session::install(core::CommandBus* bus)
{
    changes_.reset();
    installed_ = bus;
    ++revision_;
    if (bus != nullptr)
        subscribe();
}

void Session::subscribe()
{
    // Edits made elsewhere (other screens, undo from the panel) invalidate the cached views too.
    changes_ = bus().observe("", [this](const core::ChangeSet&) { ++revision_; });
}

core::CommandBus& Session::bus()
{
    if (installed_ != nullptr)
        return *installed_;
    if (! own_)
    {
        own_ = std::make_unique<core::CommandBus>();
        core::commands::replaceProject(*own_, core::model::newDefaultProject());
        subscribe();
    }
    return *own_;
}

const core::model::Project& Session::project()
{
    auto& b = bus();
    if (! core::commands::hasProject(b))
        core::commands::replaceProject(b, core::model::newDefaultProject());
    return core::commands::project(static_cast<const core::CommandBus&>(b));
}

void Session::execute(std::unique_ptr<core::Command> c)
{
    (void)project(); // a bus without a model ignores commands
    bus().execute(std::move(c));
    if (! installed_)
        core::commands::flushNotifications(bus());
    ++revision_;
}

bool Session::undo()
{
    const bool ok = bus().undo();
    if (! installed_)
        core::commands::flushNotifications(bus());
    ++revision_;
    return ok;
}

bool Session::redo()
{
    const bool ok = bus().redo();
    if (! installed_)
        core::commands::flushNotifications(bus());
    ++revision_;
    return ok;
}

void Session::reset()
{
    changes_.reset();
    installed_ = nullptr;
    own_.reset();
    ++revision_;
}

namespace
{
char sceneName[72] = {};
std::size_t sceneNameLength = 0;
} // namespace

std::string_view sceneNameText() noexcept
{
    return {sceneName, sceneNameLength};
}

void setSceneNameText(std::string_view s) noexcept
{
    sceneNameLength = std::min(s.size(), sizeof sceneName);
    std::copy_n(s.data(), sceneNameLength, sceneName);
}

float lockValue(const core::model::Pattern& p, const core::model::Step& cell, core::model::LockSlot slot,
                float fallback)
{
    if (cell.lockIndex == core::model::Step::kNoLocks || cell.lockIndex >= p.locks.size())
        return fallback;
    const auto& l = p.locks[cell.lockIndex];
    return (l.mask >> slot & 1u) != 0 ? l.values[slot] : fallback;
}

namespace
{
const core::model::Pattern& patternOf(const core::model::Project& p, int scene, int pattern) noexcept
{
    return p.scenes[static_cast<std::size_t>(scene & 15)].patterns[static_cast<std::size_t>(pattern & 15)];
}

// 1/24 steps per unit of seq.offset (its ParamSpec decides: ±24 → 1, ±1 step → 24).
float offsetScale() noexcept
{
    static const float scale = []
    {
        const auto* s = core::builtinParams().find("seq.offset");
        return s != nullptr && s->maxValue > 0 ? 24.0f / s->maxValue : 1.0f;
    }();
    return scale;
}
} // namespace

StepCellView Session::cell(int scene, int pattern, int track, int step)
{
    StepCellView v;
    if (step < 0 || step >= core::model::kSteps)
        return v;
    const auto& p = patternOf(project(), scene, pattern);
    const auto& c = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track & 15)];
    if (c.trigger != core::model::Step::kEmpty)
        v.kind = c.triggerType == core::model::TriggerType::Full   ? StepKind::Full
                 : c.triggerType == core::model::TriggerType::Half ? StepKind::Half
                                                                   : StepKind::Param;
    v.locked = c.lockIndex != core::model::Step::kNoLocks;
    // An unlocked step plays the track's values (SEQ-030/038: the ×n badge and the repeat marks follow the
    // track's RETRIGGER CNT / SPD and OFF SET).
    const auto& seq = project().scenes[static_cast<std::size_t>(scene & 15)].tracks[static_cast<std::size_t>(track & 15)].seq;
    v.offset24 = static_cast<int>(std::lround(lockValue(p, c, kSlotOffset, static_cast<float>(seq.offset)) * offsetScale()));
    v.retrigCount = static_cast<int>(std::lround(lockValue(p, c, kSlotRetrigCount, static_cast<float>(seq.retriggerCount))));
    v.retrigSpeed = static_cast<int>(std::lround(lockValue(p, c, kSlotRetrigSpeed, static_cast<float>(seq.retriggerSpeed))));
    return v;
}

int Session::patternLength(int scene, int pattern)
{
    return patternOf(project(), scene, pattern).length;
}

bool Session::trackHasTriggers(int scene, int pattern, int track)
{
    const auto& p = patternOf(project(), scene, pattern);
    for (const auto& row : p.steps)
        if (row[static_cast<std::size_t>(track & 15)].trigger != core::model::Step::kEmpty)
            return true;
    return false;
}

bool Session::patternHasContent(int scene, int pattern)
{
    return (patternContentMask(scene) >> (pattern & 15) & 1u) != 0;
}

std::uint16_t Session::patternContentMask(int scene)
{
    if (cachedRevision_ != revision_)
    {
        const auto& proj = project();
        for (int s = 0; s < core::model::kScenes; ++s)
        {
            std::uint16_t mask = 0;
            for (int p = 0; p < core::model::kPatterns; ++p)
            {
                const auto& pat = patternOf(proj, s, p);
                bool any = false;
                for (const auto& row : pat.steps)
                {
                    for (const auto& c : row)
                        any |= c.trigger != core::model::Step::kEmpty;
                    if (any)
                        break;
                }
                if (any)
                    mask = static_cast<std::uint16_t>(mask | 1u << p);
            }
            contentMask_[static_cast<std::size_t>(s)] = mask;
        }
        cachedRevision_ = revision_;
    }
    return contentMask_[static_cast<std::size_t>(scene & 15)];
}

bool Session::sceneHasContent(int scene)
{
    const auto& s = project().scenes[static_cast<std::size_t>(scene & 15)];
    if (! s.name.empty() || s.bpm.has_value())
        return true;
    return patternContentMask(scene) != 0;
}
} // namespace ui::m2
