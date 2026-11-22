#include "services/render/ProjectSamples.h"

#include "services/library/SamplePool.h"
#include "services/library/TrackSampleSync.h"
#include "services/library/Workers.h"

#include <chrono>
#include <thread>

namespace services::render
{
namespace
{
namespace lib = services::library;
namespace store = services::store;

// The sync's engine side: nothing (the render installs the snapshot itself). A refused post gives the engine's
// reference back at once (TrackSampleSync's rule).
class NullBridge final : public engine::EngineBridge
{
public:
    bool post(const engine::EngineMessage&) override { return false; }
    void apply(const core::ChangeSet&, const engine::model::Project&) override {}
    const engine::PlayheadFeed& playhead() const override { return playhead_; }
    const engine::MeterFeed& meters() const override { return meters_; }
    void retirePending() override {}
    bool popEvent(engine::EngineEvent&) override { return false; }

private:
    engine::PlayheadFeed playhead_ {};
    engine::MeterFeed meters_ {};
};
} // namespace

struct LoadedSamples::Impl
{
    store::SampleRoots roots;
    lib::Workers workers {2};
    std::unique_ptr<lib::SamplePool> pool;
    SampleSnapshot snapshot;
    std::vector<std::string> errors;
};

LoadedSamples::LoadedSamples() : impl_(std::make_unique<Impl>()) {}

LoadedSamples::~LoadedSamples()
{
    impl_->snapshot = {};
    impl_->workers.drain();
    impl_->pool.reset(); // frees every buffer nobody references any more
}

const SampleSnapshot& LoadedSamples::snapshot() const noexcept
{
    return impl_->snapshot;
}

const std::vector<std::string>& LoadedSamples::errors() const noexcept
{
    return impl_->errors;
}

core::Result<std::unique_ptr<LoadedSamples>> LoadedSamples::load(core::model::Project& p,
                                                                  const store::SampleRoots& roots, double rate,
                                                                  double timeoutSeconds)
{
    std::unique_ptr<LoadedSamples> out(new LoadedSamples());
    auto& d = *out->impl_;
    d.roots = roots;
    store::resolvePool(p.sampleRefs, d.roots);
    d.pool = std::make_unique<lib::SamplePool>(
        d.workers, [&d](const std::string& path) { return store::resolveLocation(store::splitLocation(path), d.roots); },
        lib::SamplePool::Config {.engineRate = rate});
    NullBridge none;
    lib::TrackSampleSync sync(*d.pool, none);
    sync.onError = [&d](int s, int t, const core::Error& e)
    { d.errors.push_back("scene " + std::to_string(s + 1) + " track " + std::to_string(t + 1) + ": " + e.message); };
    sync.syncAll(p);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;)
    {
        d.workers.pump();
        bool loading = false;
        for (int s = 0; s < 16 && ! loading; ++s)
            for (int t = 0; t < 16 && ! loading; ++t)
                loading = sync.loading(s, t);
        if (! loading)
            break;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeoutSeconds)
            return core::Error {"Timed out loading the samples", "LoadedSamples::load"};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    d.snapshot = SampleSnapshot::of(sync);
    return out;
}
} // namespace services::render
