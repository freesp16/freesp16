#include "engine/sequencer/perf/PerfStage.h"

namespace engine::sequencer::perf
{
namespace
{
using trigger::TriggerKind;
using trigger::TriggerSource;

std::uint8_t roundClamp(float v, int lo, int hi) noexcept
{
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    return static_cast<std::uint8_t>(i < lo ? lo : (i > hi ? hi : i));
}
} // namespace

RetrigSpec PerfStage::specOf(int scene, const trigger::TriggerEvent& step) const noexcept
{
    RetrigSpec s = trackRetrig(scene, step.track);
    if (step.locks != nullptr && step.locks->values != nullptr)
    {
        float v = 0.0f;
        if (step.locks->get(kLockSlotRetrigCount, v))
            s.count = roundClamp(v, 1, kMaxRetrigCount);
        if (step.locks->get(kLockSlotRetrigSpeed, v))
            s.speedIndex = roundClamp(v, 0, kRetrigSpeeds - 1);
    }
    return s;
}

// A FULL hit of `e.track` stops the other tracks of its choke group (TRK-020): their repeats due before it still fire,
// the rest are cancelled.
void PerfStage::cutGroup(const trigger::TriggerRouter& router, int scene, const trigger::TriggerEvent& e) noexcept
{
    const int t = e.track & 15;
    const int group = router.settings(scene, t).chokeGroup;
    if (group == 0)
        return;
    const std::uint32_t others = router.chokeMask(scene, group) & ~(1u << t) & retrig.pendingMask();
    for (std::uint32_t m = others; m != 0; m &= m - 1)
    {
        const int u = __builtin_ctz(m);
        retrig.cancel(u);
    }
}

void PerfStage::process(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                        trigger::TriggerRouter& router) noexcept
{
    emitted_ = 0;
    armedAt_.fill(-1);
    const int scene = timing.scene & 15;
    // A scene switch replaces the 16 tracks (their sources follow the new scene): the old scene's ratchets end at the
    // switch (our choice) — at the boundary frame when it falls inside this block, else at the block start.
    int sceneCut = -1;
    if (scene != scene_)
    {
        scene_ = scene;
        dirty_ = 0xFFFF; // the new scene's transpose values
        sceneCut = timing.boundary.has ? timing.boundary.frame : 0;
    }
    if (! ctx.playing || ctx.stoppedThisBlock)
    {
        retrig.cancelAll(); // STOP / pause end every ratchet
        retrig.endBlock();
        return;
    }
    if (ctx.startedThisBlock)
        retrig.cancelAll();

    // The block's step and choke points, in frame order (stable: push order within a frame). The pending events are
    // the live hits (frame 0) and the sequencer's steps just pushed; repeats pushed below land after this span.
    const auto events = router.pendingEvents();
    std::array<std::uint8_t, trigger::TriggerRouter::kMaxEvents> order; // written before read (no braces)
    int n = 0;
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto& e = events[i];
        const bool step = e.source == TriggerSource::Sequencer && (e.kind == TriggerKind::Full || e.kind == TriggerKind::Half);
        if (! step && e.kind != TriggerKind::Full && e.kind != TriggerKind::Stop)
            continue;
        if (! step && retrig.pendingMask() == 0)
            continue; // nothing to choke
        int j = n++;
        while (j > 0 && events[order[static_cast<std::size_t>(j - 1)]].frame > e.frame)
        {
            order[static_cast<std::size_t>(j)] = order[static_cast<std::size_t>(j - 1)];
            --j;
        }
        order[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(i);
    }

    const double stepSamples = stepSamplesAt(ctx.sampleRate > 0.0 ? ctx.sampleRate : sampleRate_, ctx.bpm);
    const int routerScene = router.scene();
    auto cutScene = [&] {
        for (std::uint32_t m = retrig.pendingMask(); m != 0; m &= m - 1)
            emitted_ += retrig.emitUntil(__builtin_ctz(m), sceneCut, router);
        retrig.cancelAll();
        sceneCut = -1;
    };
    for (int k = 0; k < n; ++k)
    {
        const trigger::TriggerEvent e = events[order[static_cast<std::size_t>(k)]];
        const int t = e.track & 15;
        if (sceneCut >= 0 && e.frame >= sceneCut)
            cutScene(); // the old pattern's steps come before the boundary, the new scene's from it on
        if (e.kind == TriggerKind::Stop)
        {
            emitted_ += retrig.emitUntil(t, e.frame, router);
            retrig.cancel(t);
            continue;
        }
        if (e.kind == TriggerKind::Full && retrig.pendingMask() != 0)
        {
            // Repeats of the other group members due before this hit still fire (the router chokes them in order).
            const int group = router.settings(routerScene, t).chokeGroup;
            if (group != 0)
                for (std::uint32_t m = router.chokeMask(routerScene, group) & ~(1u << t) & retrig.pendingMask(); m != 0;
                     m &= m - 1)
                    emitted_ += retrig.emitUntil(__builtin_ctz(m), e.frame, router);
            cutGroup(router, routerScene, e);
        }
        if (e.source == TriggerSource::Sequencer && (e.kind == TriggerKind::Full || e.kind == TriggerKind::Half))
        {
            emitted_ += retrig.emitUntil(t, e.frame, router); // the previous step's repeats before this one
            retrig.onStep(e, specOf(scene_, e), stepSamples);
            armedAt_[static_cast<std::size_t>(t)] = static_cast<std::int8_t>(e.frame);
        }
    }
    if (sceneCut >= 0)
        cutScene();
    emitted_ += retrig.emitBlock(router);
}

void PerfStage::afterRouter(const trigger::TriggerRouter& router) noexcept
{
    if (emitted_ == 0 || retrig.pendingMask() == 0)
        return;
    const int scene = router.scene();
    for (const auto& e : router.blockEvents())
    {
        if (e.source != TriggerSource::Retrigger || e.kind != TriggerKind::Full)
            continue;
        const int t = e.track & 15;
        const int group = router.settings(scene, t).chokeGroup;
        if (group == 0)
            continue;
        for (std::uint32_t m = router.chokeMask(scene, group) & ~(1u << t) & retrig.pendingMask(); m != 0; m &= m - 1)
        {
            const int u = __builtin_ctz(m);
            if (armedAt_[static_cast<std::size_t>(u)] <= e.frame) // re-armed after this repeat: keep
                retrig.cancel(u);
        }
    }
}
} // namespace engine::sequencer::perf
