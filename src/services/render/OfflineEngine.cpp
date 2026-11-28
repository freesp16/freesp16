#include "services/render/OfflineEngine.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "services/library/TrackSampleSync.h"
#include "services/plugin/ClapHost.h"

#include <algorithm>

namespace services::render
{
// ---------------------------------------------------------------------------------------------- SampleSnapshot

SampleSnapshot::~SampleSnapshot()
{
    releaseAll();
}

SampleSnapshot::SampleSnapshot(const SampleSnapshot& o) : slots_(o.slots_)
{
    for (const auto h : slots_)
        if (h != nullptr)
            core::audio::retain(h);
}

SampleSnapshot& SampleSnapshot::operator= (const SampleSnapshot& o)
{
    if (this != &o)
    {
        for (const auto h : o.slots_)
            if (h != nullptr)
                core::audio::retain(h);
        releaseAll();
        slots_ = o.slots_;
    }
    return *this;
}

SampleSnapshot::SampleSnapshot(SampleSnapshot&& o) noexcept : slots_(o.slots_)
{
    o.slots_.fill(nullptr);
}

SampleSnapshot& SampleSnapshot::operator= (SampleSnapshot&& o) noexcept
{
    if (this != &o)
    {
        releaseAll();
        slots_ = o.slots_;
        o.slots_.fill(nullptr);
    }
    return *this;
}

void SampleSnapshot::set(int scene, int track, SampleHandle h) noexcept
{
    auto& slot = slots_[index(scene, track)];
    if (h != nullptr)
        core::audio::retain(h);
    if (slot != nullptr)
        core::audio::release(slot);
    slot = h;
}

int SampleSnapshot::count() const noexcept
{
    return static_cast<int>(std::count_if(slots_.begin(), slots_.end(), [](SampleHandle h) { return h != nullptr; }));
}

void SampleSnapshot::releaseAll() noexcept
{
    for (auto& h : slots_)
        if (h != nullptr)
        {
            core::audio::release(h);
            h = nullptr;
        }
}

SampleSnapshot SampleSnapshot::of(const services::library::TrackSampleSync& sync)
{
    SampleSnapshot s;
    for (int sc = 0; sc < 16; ++sc)
        for (int t = 0; t < 16; ++t)
            s.set(sc, t, sync.current(sc, t));
    return s;
}

// ---------------------------------------------------------------------------------------------- OfflineEngine

engine::EngineSetup builtinEngineSetup()
{
    const auto& params = core::builtinParams();
    engine::EngineSetup setup {[&params](std::string_view id) { return params.idOf(id).value_or(core::ParamId {}); }};
    for (const auto& spec : params.all())
        setup.paramIds.push_back(spec.id);
    setup.fxInternalIndex = core::fx::internalIndexTable(params);
    return setup;
}

namespace
{
std::unique_ptr<engine::AudioEngine> makeEngine(double rate)
{
    auto e = engine::createEngine(builtinEngineSetup()); // the app's engine, no device
    e->prepare(rate, 512);
    // No TEMPORARY test tone in an offline render (an empty track 1 would otherwise put 440 Hz into the file).
    if (const auto id = core::builtinParams().idOf("render.offline"))
        e->bridge().post(engine::ParamChange {*id, 0, 1.0f, 0});
    return e;
}
} // namespace

OfflineEngine::OfflineEngine(Config c)
    : config_(c), engine_(makeEngine(c.sampleRate)), queue_(engine_->bridge(), 512) // the engine drains ≤ 512 per block
{
    config_.outPorts = std::clamp(config_.outPorts, 1, static_cast<int>(engine::OutPort::Count));
    buffers_.resize(static_cast<std::size_t>(2 * config_.outPorts));
    for (auto& b : buffers_)
    {
        b.fill(0.0f);
        pointers_.push_back(b.data());
    }
}

// Members go in reverse order: the queue first (it gives back the swaps it still holds), then the engine (it frees
// what it installed).
OfflineEngine::~OfflineEngine() = default;

void OfflineEngine::begin(const core::model::Project& p, const SampleSnapshot& samples, const ProjectFeeder& feed,
                          core::arrangement::SongStatus* status)
{
    plugins_ = std::make_unique<plugin::ClapHostService>(queue_, config_.pluginsDir, config_.sampleRate);
    plugins_->syncFromProject(p);
    plugins_->pollMainThread();
    if (feed)
        feed(p, queue_, status, samples);
    for (int sc = 0; sc < 16; ++sc)
        for (int t = 0; t < 16; ++t)
            if (const auto h = samples.at(sc, t))
            {
                core::audio::retain(h); // the engine's slot reference (its TrackSample destroy function releases it)
                if (! queue_.post(engine::ObjectSwap {{engine::SwapSlot::TrackSample, static_cast<std::uint8_t>(sc),
                                                       static_cast<std::uint8_t>(t)},
                                                      h}))
                    core::audio::release(h);
            }
}

int OfflineEngine::settle()
{
    int preroll = 0;
    // Stopped: each block drains the FIFO and installs the swaps. Bounded (a refusing engine cannot hang a render).
    while ((! queue_.flush() || preroll < 8) && preroll < 1000000)
    {
        engine_->process({pointers_.data(), config_.outPorts, nullptr, 0, engine::kBlockFrames, 0});
        ++blocks_;
        ++preroll;
    }
    for (int i = 0; i < 8; ++i)
        process(); // settle the strip ramps (silence: nothing plays)
    engine::EngineEvent ev;
    while (engine_->bridge().popEvent(ev))
    {
    }
    return preroll;
}

void OfflineEngine::process() noexcept
{
    // This worker owns the offline CLAP instances. Give callbacks time between processing blocks.
    if (plugins_ && blocks_ % 16 == 0)
        plugins_->pollMainThread();
    queue_.flush();
    engine_->process({pointers_.data(), config_.outPorts, nullptr, 0, engine::kBlockFrames, 0});
    ++blocks_;
}

std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t h) noexcept
{
    const auto* b = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
} // namespace services::render
