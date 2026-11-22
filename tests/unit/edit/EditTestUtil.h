// Helpers of the edit tests: a CommandBus with a project full of generated content (triggers, locks,
// distinct track settings, scene names / colours / BPMs), a model hash (FNV-1a over the native codec's encoding, so
// "undo = original" is checked on the saved form too), and a worker pool the test drives by hand.
#pragma once

#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "core/edit/WorkerPool.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "services/store/NativeCodec.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace edittest
{
namespace m = core::model;

inline std::uint64_t fnv(std::uint64_t h, const std::string& s) noexcept
{
    for (const unsigned char c : s)
        h = (h ^ c) * 0x100000001b3ull;
    return h;
}

// FNV-1a of every file the native codec writes for `p` (names and text).
inline std::uint64_t hash(const m::Project& p)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    services::store::SaveOptions o;
    o.timestamp = "2026-01-01T00:00:00Z"; // pinned: meta.json would carry "now"
    for (const auto& f : services::store::NativeCodec::encode(p, o))
        h = fnv(fnv(h, f.name), f.text);
    return h;
}

// A deterministic pseudo-random cell (xorshift).
struct Rng
{
    std::uint32_t s = 0x12345678u;
    std::uint32_t next() noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) noexcept { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
};

// Sets a trigger on (step, track) of `p`, optionally with a lock record (slot `slot` = value).
inline void putStep(m::Pattern& p, int step, int track, int velocity, int lockSlot = -1, float value = 0.0f)
{
    auto& cell = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    cell.trigger = 15;
    cell.triggerType = m::TriggerType::Full;
    cell.value2 = static_cast<std::int16_t>(velocity);
    if (lockSlot >= 0)
    {
        m::StepLocks l;
        l.mask = std::uint64_t {1} << lockSlot;
        l.values[static_cast<std::size_t>(lockSlot)] = value;
        p.locks.push_back(l);
        cell.lockIndex = static_cast<std::uint16_t>(p.locks.size() - 1);
    }
}

// Fills scene `s`: distinct track settings, triggers with locks in every pattern (density ≈ 1/4), name, BPM.
inline void fillScene(m::Scene& scene, int s, Rng& rng)
{
    scene.name = "Scene " + std::to_string(s + 1);
    scene.colour = (s * 5 + 3) & 15;
    scene.bpm = 100.0 + s;
    for (int t = 0; t < m::kTracks; ++t)
    {
        auto& tr = scene.tracks[static_cast<std::size_t>(t)];
        tr.volume = 20 + rng.below(100);
        tr.pan = rng.below(129) - 64;
        tr.chokeGroup = rng.below(9);
        tr.mute = rng.below(5) == 0;
        tr.name = "T" + std::to_string(s) + "." + std::to_string(t);
    }
    for (int pi = 0; pi < m::kPatterns; ++pi)
    {
        auto& p = scene.patterns[static_cast<std::size_t>(pi)];
        p.length = 16 * (1 + rng.below(4));
        p.trackMutes[static_cast<std::size_t>(rng.below(16))] = true;
        p.trackSolos[static_cast<std::size_t>(rng.below(16))] = rng.below(2) == 0;
        for (int st = 0; st < m::kSteps; ++st)
            for (int t = 0; t < m::kTracks; ++t)
                if (rng.below(4) == 0)
                    putStep(p, st, t, 1 + rng.below(127), rng.below(3) == 0 ? rng.below(m::kLockSlots) : -1,
                            static_cast<float>(rng.below(1000)) * 0.1f);
    }
}

struct Bus
{
    core::CommandBus bus;
    explicit Bus(bool content = true, int scenes = 4)
    {
        core::commands::replaceProject(bus, m::newDefaultProject());
        if (content)
        {
            Rng rng;
            for (int s = 0; s < scenes; ++s)
                fillScene(project().scenes[static_cast<std::size_t>(s)], s, rng); // set-up, not an edit
        }
    }
    m::Project& project() { return core::commands::project(bus); }
    const m::Pattern& pattern(int s, int p) { return project().scenes[static_cast<std::size_t>(s)].patterns[static_cast<std::size_t>(p)]; }
    const m::Track& track(int s, int t) { return project().scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)]; }
    std::size_t undoDepth() { return core::commands::history(bus).undoDepth(); }
};

// The cell (step, track) of `p` with its lock record resolved (equality across patterns ignores lock indices).
struct CellView
{
    m::Step step;
    bool hasLocks = false;
    m::StepLocks locks {};
    friend bool operator== (const CellView& a, const CellView& b)
    {
        return a.step.trigger == b.step.trigger && a.step.value2 == b.step.value2 &&
               a.step.triggerType == b.step.triggerType && a.hasLocks == b.hasLocks && (! a.hasLocks || a.locks == b.locks);
    }
};
inline CellView cell(const m::Pattern& p, int step, int track)
{
    CellView v;
    v.step = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    if (v.step.lockIndex != m::Step::kNoLocks && v.step.lockIndex < p.locks.size())
    {
        v.hasLocks = true;
        v.locks = p.locks[v.step.lockIndex];
    }
    return v;
}

// A pool whose jobs run when the test says so (on the test thread, like a worker that has not got to them yet).
class ManualPool final : public core::edit::WorkerPool
{
public:
    void run(std::function<void()> job, std::function<void()> done) override
    {
        queue.emplace_back(std::move(job), std::move(done));
    }
    // Runs the job and completion of entry i (0 = oldest) and removes it.
    void finish(std::size_t i = 0)
    {
        auto [job, done] = std::move(queue[i]);
        queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
        if (job)
            job();
        if (done)
            done();
    }
    void finishAll()
    {
        while (! queue.empty())
            finish(0);
    }
    std::deque<std::pair<std::function<void()>, std::function<void()>>> queue;
};
} // namespace edittest
