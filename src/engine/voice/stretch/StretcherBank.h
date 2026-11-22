// The engine's 16 time-stretchers (one per track voice) of the selected backend, the swap that changes the backend,
// the per-block governor and the priming budget (audio.stretchBackend).
//
//   message thread   prepare(rate, block)   builds the selected backend's set (device stopped: installed at once)
//                    select(name, log)      builds a new set (allocates) and queues it; the audio thread installs it
//                                           at its next block start with one pointer exchange (RT-safe, the ObjectSwap
//                                           pattern without a SwapSlot kind); logs "STRETCH backend=…"
//                    collect()              frees a set the audio thread retired (call after select / periodically)
//                    setSampleTempo(…)      the sample BPM of [scene][track] (sidecar bpm / bars, PLY-031); any thread
//   audio thread     beginBlock(fx)         installs a queued set, runs the governor on the FX load and on the number
//                                           of MASTER TEMPO voices of the previous block, refills the priming budget
//                    lane(t), tryPrime(), countPlayer(), holdNew(), scratch
//
// Backends: "pvoc" (own phase vocoder), "wsola" (own time-domain), "none" (resample only = MASTER TEMPO plays as
// RESAMPLE), and any other APP_REGISTER_STRETCHER name. The default, provisional until a device bench and a listening
// test decide: kDefaultBackend. Priming: at most kMaxPrimesPerBlock MASTER TEMPO
// note starts per block; further starts wait for the next block (≤ 1.45 ms late).
#pragma once

#include "engine/voice/TimeStretcher.h"
#include "engine/voice/stretch/StretchGovernor.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::voice::stretch
{
class GrainStretcher;

class StretcherBank
{
public:
    static constexpr int kLanes = 16;
    static constexpr int kMaxPrimesPerBlock = 2;
    static constexpr std::string_view kDefaultBackend = "wsola"; // provisional
    using Log = std::function<void(const std::string&)>; // rt-ok: message-thread logging hook

    StretcherBank() = default;
    ~StretcherBank();
    StretcherBank(const StretcherBank&) = delete;
    StretcherBank& operator= (const StretcherBank&) = delete;

    // ---- message thread ----
    void prepare(double sampleRate, int maxBlock);
    bool select(std::string_view backend, const Log& log = {}); // false: unknown name (nothing changes)
    void collect() noexcept;
    std::string selected() const { return selected_; } // rt-ok: message thread
    void setGovernorConfig(const GovernorConfig& c) noexcept { config_ = c; } // before the device starts
    // ---- any thread ----
    void setSampleTempo(int scene, int track, float bpm) noexcept;
    float sampleTempo(int scene, int track) const noexcept
    {
        return tempo_[static_cast<std::size_t>((scene & 15) * kLanes + (track & 15))].load(std::memory_order_relaxed);
    }
    bool holding() const noexcept { return holdFlag_.load(std::memory_order_relaxed); } // the caution chip
    int tierNow() const noexcept { return tierFlag_.load(std::memory_order_relaxed); }
    std::uint32_t installs() const noexcept { return installs_.load(std::memory_order_relaxed); }

    // ---- audio thread ----
    void beginBlock(const GovernorInput& fx) noexcept;
    TimeStretcher* lane(int track) noexcept;           // null when the backend is "none" or nothing is prepared
    std::uint32_t generation() const noexcept { return generation_; } // changes with every installed set
    bool tryPrime() noexcept { return primes_ > 0 ? (--primes_, true) : false; }
    void countPlayer() noexcept { ++players_; }
    bool holdNew() const noexcept { return decision_.holdNew; }
    const GovernorDecision& decision() const noexcept { return decision_; }
    // The shared input scratch of the voices (one voice renders at a time): 2 × scratchFrames().
    float* scratch(int c) noexcept;
    int scratchFrames() const noexcept;
    std::string_view installedName() const noexcept;

private:
    struct Set;
    std::unique_ptr<Set> build(std::string_view backend) const;
    void install(Set* s) noexcept;

    double sampleRate_ = 44100.0;
    int maxBlock_ = 64;
    bool prepared_ = false;
    std::string selected_ {kDefaultBackend}; // rt-ok: message thread only
    GovernorConfig config_ {};

    Set* current_ = nullptr;                 // audio thread (and prepare, device stopped)
    std::atomic<Set*> pending_ {nullptr};    // message → audio
    std::atomic<Set*> retired_ {nullptr};    // audio → message
    std::atomic<std::uint32_t> installs_ {0};
    std::uint32_t generation_ = 0;

    GovernorDecision decision_ {};
    int players_ = 0, primes_ = kMaxPrimesPerBlock;
    std::atomic<bool> holdFlag_ {false};
    std::atomic<int> tierFlag_ {0};
    std::array<std::atomic<float>, 16 * kLanes> tempo_ {};
};
} // namespace engine::voice::stretch
