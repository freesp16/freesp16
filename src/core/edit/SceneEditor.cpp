#include "core/edit/SceneEditor.h"

#include "core/commands/CommandBusAccess.h"
#include "core/edit/EditOps.h"

#include <algorithm>
#include <utility>

namespace core::edit
{
// One paste in flight: the worker fills `cmd`; the message thread installs it when it and every earlier one are done.
struct SceneEditor::Job
{
    std::uint64_t id = 0;
    EditRef dst {};
    const void* project = nullptr;
    double startMs = 0;
    Done done;
    std::shared_ptr<const ClipPayload> clip;
    std::unique_ptr<Command> cmd; // written by the worker, read on the message thread after `finished`
    bool ready = false;
};

SceneEditor::SceneEditor(CommandBus& bus, WorkerPool& workers, const Clock* clock)
    : bus_(bus), workers_(&workers), clock_(clock != nullptr ? clock : &steady_), self_(std::make_shared<SceneEditor*>(this))
{
}

SceneEditor::~SceneEditor()
{
    self_.reset(); // completions arriving later find the editor gone (the worker only touched its own Job)
}

const void* SceneEditor::projectId() const noexcept
{
    return commands::hasProject(bus_) ? static_cast<const void*>(&commands::project(bus_)) : nullptr;
}

void SceneEditor::copy(const EditRef& src)
{
    if (! commands::hasProject(bus_))
        return;
    if (auto clip = snapshot(commands::project(bus_), src))
        clip_.set(std::move(clip));
}

bool SceneEditor::canPaste(const EditRef& dst) const noexcept
{
    const auto clip = clip_.payload();
    return clip != nullptr && projectId() != nullptr && core::edit::canPaste(*clip, dst, projectId());
}

bool SceneEditor::isCopied(const EditRef& ref) const noexcept
{
    const EditRef* s = clip_.source();
    if (s == nullptr || familyOf(s->kind) != familyOf(ref.kind))
        return false;
    switch (familyOf(ref.kind))
    {
        case Family::Scene: return s->scene == ref.scene;
        case Family::Pattern: return s->scene == ref.scene && s->pattern == ref.pattern;
        case Family::Track: return s->scene == ref.scene && s->track == ref.track;
        case Family::Sequence: return s->scene == ref.scene && s->pattern == ref.pattern && s->track == ref.track;
        case Family::Steps: break;
    }
    return s->scene == ref.scene && s->pattern == ref.pattern && s->track == ref.track && s->stepMask == ref.stepMask;
}

void SceneEditor::paste(const EditRef& dst, Done done)
{
    if (! canPaste(dst))
    {
        if (done)
            done(Result::failure(clip_.empty() ? "The clipboard is empty." : "The clipboard holds another kind."));
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = nextJob_++;
    job->dst = dst;
    job->project = projectId();
    job->startMs = clock_->nowMs();
    job->done = std::move(done);
    job->clip = clip_.payload();
    jobs_.push_back(job);
    const std::uint64_t id = job->id;
    // The worker sees only the Job (the immutable payload and the destination); the model stays on this thread.
    workers_->run([job] { job->cmd = preparePaste(job->clip, job->dst); },
                 [weak = std::weak_ptr<SceneEditor*>(self_), id] {
                     if (auto alive = weak.lock())
                         (*alive)->finished(id);
                 });
}

void SceneEditor::finished(std::uint64_t id)
{
    for (auto& j : jobs_)
        if (j->id == id)
            j->ready = true;
    installReady();
}

void SceneEditor::installReady()
{
    while (! jobs_.empty() && jobs_.front()->ready)
    {
        const std::shared_ptr<Job> job = std::move(jobs_.front());
        jobs_.pop_front();
        Result result;
        if (! job->cmd)
            result = Result::failure("Paste failed.");
        else if (projectId() != job->project)
            result = Result::failure("The project changed before the paste finished.");
        else
            bus_.execute(std::move(job->cmd)); // one undo step; the bridge hook makes it audible at the next block
        job->clip.reset();
        if (job->done)
            job->done(result);
    }
}

bool SceneEditor::busyIndicator(double nowMs) const noexcept
{
    return ! jobs_.empty() && nowMs - jobs_.front()->startMs > kBusyAfterMs;
}

void SceneEditor::swapTracks(int scene, int a, int b, bool withSequences)
{
    if ((a & 15) == (b & 15))
        return;
    bus_.execute(std::make_unique<SwapTracksCommand>(scene, a, b, withSequences));
}

bool SceneEditor::swap(const EditRef& a, const EditRef& b)
{
    auto cmd = makeSwap(a, b);
    if (! cmd)
        return false;
    bus_.execute(std::move(cmd));
    return true;
}

void SceneEditor::reset(const EditRef& target)
{
    if (auto cmd = makeReset(target))
        bus_.execute(std::move(cmd));
}
} // namespace core::edit
