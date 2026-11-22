#include "engine/voice/stretch/StretcherBank.h"

#include "engine/voice/stretch/GrainStretcher.h"

#include <algorithm>
#include <cstdio>

namespace engine::voice::stretch
{
struct StretcherBank::Set
{
    std::string name; // rt-ok: built on the message thread, only read (installedName) on the audio thread
    bool none = false; // "none": MASTER TEMPO plays as RESAMPLE, no lane is used
    std::array<std::unique_ptr<TimeStretcher>, kLanes> lanes;
    std::array<GrainStretcher*, kLanes> grain {}; // the own backends' quality tier switch (null for others)
    std::vector<float> scratch[2];
    int scratchFrames = 0;
};

StretcherBank::~StretcherBank()
{
    delete current_;                    // rt-ok: destructor, message thread (device stopped)
    delete pending_.exchange(nullptr);  // rt-ok: destructor, message thread
    delete retired_.exchange(nullptr);  // rt-ok: destructor, message thread
}

std::unique_ptr<StretcherBank::Set> StretcherBank::build(std::string_view backend) const
{
    const auto create = StretcherRegistry::builtin().find(backend);
    if (create == nullptr)
        return nullptr;
    auto set = std::make_unique<Set>(); // rt-ok: build(), message thread
    set->name = std::string(backend);   // rt-ok: build(), message thread
    set->none = backend == "none";
    int need = 4 * maxBlock_;
    for (int t = 0; t < kLanes; ++t)
    {
        auto s = create();
        if (! s)
            return nullptr;
        s->prepare(sampleRate_, maxBlock_, 2);
        auto* g = dynamic_cast<GrainStretcher*>(s.get());
        set->grain[static_cast<std::size_t>(t)] = g;
        if (g != nullptr)
            g->setComputeAhead(t * maxBlock_); // stagger the grains of voices started together
        // The largest pull of one call over the ratio range and both tiers (the first call after a reset).
        if (t == 0 || g != nullptr)
            for (int tier = 0; tier < 2; ++tier)
            {
                if (g != nullptr)
                    g->setQuality(tier);
                for (const double r : {0.25, 0.5, 1.0, 2.0, 4.0})
                    for (const double p : {0.5, 1.0, 2.0})
                    {
                        s->setRatios(r, p);
                        s->reset();
                        need = std::max(need, s->framesNeeded(maxBlock_));
                    }
            }
        if (g != nullptr)
            g->setQuality(0);
        s->setRatios(1.0, 1.0);
        s->reset();
        set->lanes[static_cast<std::size_t>(t)] = std::move(s);
    }
    set->scratchFrames = need + maxBlock_;
    for (auto& v : set->scratch)
        v.assign(static_cast<std::size_t>(set->scratchFrames), 0.0f);
    return set;
}

void StretcherBank::prepare(double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlock_ = std::max(1, maxBlock);
    prepared_ = true;
    collect();
    delete pending_.exchange(nullptr); // rt-ok: prepare(), message thread (device stopped)
    auto set = build(selected_);
    if (! set)
    {
        selected_ = std::string(kDefaultBackend); // rt-ok: prepare(), message thread
        set = build(selected_);
    }
    delete current_; // rt-ok: prepare(), message thread, device stopped: install directly
    current_ = set.release();
    ++generation_;
    installs_.fetch_add(1, std::memory_order_relaxed);
}

bool StretcherBank::select(std::string_view backend, const Log& log)
{
    if (StretcherRegistry::builtin().find(backend) == nullptr)
    {
        if (log)
            log("STRETCH backend=" + std::string(backend) + " unknown, keeping " + selected_); // rt-ok: message thread
        return false;
    }
    selected_ = std::string(backend); // rt-ok: select(), message thread
    collect();
    if (prepared_)
    {
        auto set = build(selected_);
        if (! set)
            return false;
        // A set queued earlier and never installed is replaced.
        delete pending_.exchange(set.release()); // rt-ok: select(), message thread
    }
    if (log)
    {
        char line[160];
        std::snprintf(line, sizeof line, "STRETCH backend=%s lanes=%d rate=%.0f prime/block=%d (%s)", selected_.c_str(),
                      kLanes, sampleRate_, kMaxPrimesPerBlock,
                      backend == "none" ? "MASTER TEMPO plays as RESAMPLE" : "MASTER TEMPO time-stretch");
        log(line);
    }
    return true;
}

void StretcherBank::collect() noexcept
{
    delete retired_.exchange(nullptr); // rt-ok: collect(), message thread
}

void StretcherBank::setSampleTempo(int scene, int track, float bpm) noexcept
{
    tempo_[static_cast<std::size_t>((scene & 15) * kLanes + (track & 15))].store(bpm > 0.0f ? bpm : 0.0f,
                                                                               std::memory_order_relaxed);
}

void StretcherBank::install(Set* s) noexcept
{
    Set* old = current_;
    current_ = s;
    ++generation_;
    installs_.fetch_add(1, std::memory_order_relaxed);
    retired_.store(old, std::memory_order_release);
}

void StretcherBank::beginBlock(const GovernorInput& fx) noexcept
{
    if (retired_.load(std::memory_order_acquire) == nullptr) // the previous set has been collected
        if (Set* s = pending_.exchange(nullptr, std::memory_order_acq_rel))
            install(s);
    GovernorInput in = fx;
    in.masterTempoVoices = players_;
    decision_ = govern(in, config_);
    players_ = 0;
    primes_ = kMaxPrimesPerBlock;
    if (current_ != nullptr)
        for (auto* g : current_->grain)
            if (g != nullptr)
                g->setQuality(decision_.tier); // from each lane's next note
    holdFlag_.store(decision_.holdNew, std::memory_order_relaxed);
    tierFlag_.store(decision_.tier, std::memory_order_relaxed);
}

TimeStretcher* StretcherBank::lane(int track) noexcept
{
    if (current_ == nullptr || current_->none)
        return nullptr;
    return current_->lanes[static_cast<std::size_t>(track & 15)].get();
}

float* StretcherBank::scratch(int c) noexcept
{
    return current_ != nullptr ? current_->scratch[c & 1].data() : nullptr;
}

int StretcherBank::scratchFrames() const noexcept
{
    return current_ != nullptr ? current_->scratchFrames : 0;
}

std::string_view StretcherBank::installedName() const noexcept
{
    return current_ != nullptr ? std::string_view(current_->name) : std::string_view();
}
} // namespace engine::voice::stretch
