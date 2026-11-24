// The MIDI wire between the threads (spec 01-architecture/threading.md "Channels between threads",
// 02-features/midi.md MID-001/070, sync-and-transport.md SYN-011/020).
//
//   MIDI port thread (io/midi, one per input) ──pushIn (SPSC per port)──────▶ audio thread (engine/midi)
//                                             ──pushForward (SPSC per port)─▶ MIDI sender thread (merge / thru, MID-002)
//   audio thread ──pushOut (SPSC, sample-stamped)──────────────────────────▶ MIDI sender thread (io/midi/MidiSender)
//   audio thread ──publishAnchor (seqlock, once per device period)─────────▶ sender (sample time → monotonic time)
//   audio thread ──pushUi (SPSC)───────────────────────────────────────────▶ message thread (CC → model, activity)
//
// The hub is the one rendezvous between engine/ (which may include core/util only) and io/ + platform/ (which may
// include engine/EngineApi.h only): it lives here, in core/util, and holds nothing but lock-free channels,
// atomics and the published CC table. engine::midi::MidiEngine owns one; the first engine constructed claims it as
// the process's live hub (claimLiveHub), the app's MIDI source finds it with liveHub() (the composition root
// cannot name engine::Engine, and AudioEngine has no MIDI accessor).
//
// Real-time: every audio-thread member is noexcept, wait-free and allocation-free; payloads are trivially copyable.
#pragma once

#include "core/util/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace core::midi
{
constexpr int kMaxPorts = 8;               // input ports and output ports, each (DIN + USB on the device; 8 is ours)
constexpr std::size_t kInQueue = 512;      // per input port: ≥ 160 ms of a saturated 31.25 kbit/s DIN stream
constexpr std::size_t kOutQueue = 2048;    // engine → sender (all ports)
constexpr std::size_t kUiQueue = 1024;     // engine → message thread

// Output port modes (MID-002; preference midi.portMode#p): 0 OUT, 1 OUT + MERGE A, 2 OUT + MERGE B, 3 THRU A, 4 THRU B,
// A = input port 0 (the unit's DIN, "MIDI IN"), B = input port 1 (its USB-B, "USB IN"). The USB port (output 1) has
// only OUT and OUT + MIDI IN (manual 1.60 p.12 "MIDI OUT MODE (USB)"); any other mode stored for it means the nearest
// of the two: OUT + USB IN (its own input) → OUT, THRU (MIDI IN) → OUT + MIDI IN, THRU (USB IN) → OUT.
constexpr int kUsbPort = 1;
constexpr int legalPortMode(int port, int mode) noexcept
{
    mode = mode < 0 ? 0 : (mode > 4 ? 4 : mode);
    if (port != kUsbPort)
        return mode;
    return mode == 1 || mode == 3 ? 1 : 0;
}

// One parsed channel / system message as it arrived (port thread: parse + timestamp only). size 1–3; SysEx is not
// carried (no SysEx receive, research docs/midi-implementation.md "Remaining gaps").
struct MidiEvent
{
    std::uint8_t port = 0, size = 0;
    std::uint8_t bytes[3] {};
    std::int64_t hostTimeNs = 0; // CLOCK_MONOTONIC at arrival
};

// One outgoing message, stamped with the engine sample time it belongs to (MID-070). port = output port 0–7.
struct MidiOutEvent
{
    std::uint8_t port = 0, size = 0;
    std::uint8_t bytes[3] {};
    std::int64_t sampleTime = 0; // engine frame index (frames rendered since prepare)
};

// The audio clock as the sender sees it: engine frame `frame` leaves the engine at monotonic time `hostNs`
// (period-start anchor, filtered by the engine's DLL), and one frame lasts `nsPerFrame`.
struct ClockAnchor
{
    std::int64_t frame = 0;
    std::int64_t hostNs = 0;
    double nsPerFrame = 1.0e9 / 44100.0;
    bool valid = false;
    // Monotonic time of engine frame `f` (fractional frames allowed).
    double timeOf(double f) const noexcept { return static_cast<double>(hostNs) + (f - static_cast<double>(frame)) * nsPerFrame; }
};

// Engine → message thread: an incoming CC the engine applied (the UI writes the model with a non-undoable, coalesced
// command), MIDI activity per port (LEDs), and the followed tempo.
struct UiEvent
{
    enum class Kind : std::uint8_t { Param, Activity, Tempo, SceneSelect } kind = Kind::Param;
    std::uint8_t port = 0;
    std::uint16_t scope = 0;  // Param: the ParamChange scope; SceneSelect: scene × 16 + pattern
    std::uint16_t param = 0;  // Param: ParamId.value
    float value = 0.0f;       // Param: the applied value; Tempo: BPM (display, rounded with hysteresis)
};

struct CcTable; // core/util/midi/CcTable.h

class MidiHub
{
public:
    // ---- port threads (exactly one producer per input port) ----
    bool pushIn(int port, const MidiEvent& e) noexcept
    {
        if (! valid(port) || ! in_[static_cast<std::size_t>(port)].push(e))
            return count(inDropped_);
        return true;
    }
    bool pushForward(int port, const MidiEvent& e) noexcept
    {
        if (! valid(port) || ! forward_[static_cast<std::size_t>(port)].push(e))
            return count(forwardDropped_);
        return true;
    }

    // ---- audio thread ----
    bool popIn(int port, MidiEvent& e) noexcept { return valid(port) && in_[static_cast<std::size_t>(port)].pop(e); }
    bool pushOut(const MidiOutEvent& e) noexcept { return out_.push(e) || count(outDropped_); }
    bool pushUi(const UiEvent& e) noexcept { return ui_.push(e) || count(uiDropped_); }
    void publishAnchor(const ClockAnchor& a) noexcept
    {
        // Seqlock: odd = writing. One writer (the audio thread); readers retry.
        const auto s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        frame_.store(a.frame, std::memory_order_relaxed);
        hostNs_.store(a.hostNs, std::memory_order_relaxed);
        nsPerFrame_.store(a.nsPerFrame, std::memory_order_relaxed);
        valid_.store(a.valid, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    // ---- sender thread ----
    bool popOut(MidiOutEvent& e) noexcept { return out_.pop(e); }
    bool popForward(int port, MidiEvent& e) noexcept
    {
        return valid(port) && forward_[static_cast<std::size_t>(port)].pop(e);
    }
    ClockAnchor anchor() const noexcept
    {
        ClockAnchor a;
        for (;;)
        {
            const auto s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u)
                continue;
            a.frame = frame_.load(std::memory_order_relaxed);
            a.hostNs = hostNs_.load(std::memory_order_relaxed);
            a.nsPerFrame = nsPerFrame_.load(std::memory_order_relaxed);
            a.valid = valid_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s0)
                return a;
        }
    }

    // ---- message thread ----
    bool popUi(UiEvent& e) noexcept { return ui_.pop(e); }
    // The fixed CC map (built from the parameter registry, io/midi/CcTableBuilder). Set once before the engine runs or
    // replaced while it runs: the previous table must outlive the engine (its owner keeps it).
    void setCcTable(const CcTable* t) noexcept { cc_.store(t, std::memory_order_release); }
    // Output port state: bit p = output port p is connected; a reconnect (or a mode change) bumps the port's
    // generation, so the engine's hanging-note guard sends Note Off for that port's sounding notes (MID-047).
    void setOutputConnected(int port, bool connected) noexcept
    {
        if (! valid(port))
            return;
        const auto bit = 1u << port;
        if (connected)
            outputs_.fetch_or(bit, std::memory_order_relaxed);
        else
            outputs_.fetch_and(~bit, std::memory_order_relaxed);
        bumpGeneration(port);
    }
    void bumpGeneration(int port) noexcept
    {
        if (valid(port))
            generation_[static_cast<std::size_t>(port)].fetch_add(1, std::memory_order_release);
    }
    void setInputConnected(int port, bool connected) noexcept
    {
        if (! valid(port))
            return;
        const auto bit = 1u << port;
        if (connected)
            inputs_.fetch_or(bit, std::memory_order_relaxed);
        else
            inputs_.fetch_and(~bit, std::memory_order_relaxed);
    }

    // Output port settings the sender needs (the engine stores them from its ParamChanges, any thread may write):
    // mode 0 OUT, 1 OUT + MERGE A, 2 OUT + MERGE B, 3 THRU A, 4 THRU B (MID-002); offset −50…+50 ms (MID-070);
    // the audio output latency added to every deadline.
    void setPortMode(int port, int mode) noexcept
    {
        if (valid(port))
            mode_[static_cast<std::size_t>(port)].store(static_cast<std::int8_t>(mode), std::memory_order_relaxed);
    }
    void setPortOffsetMs(int port, int ms) noexcept
    {
        if (valid(port))
            offset_[static_cast<std::size_t>(port)].store(static_cast<std::int16_t>(ms), std::memory_order_relaxed);
    }
    void setOutputLatencyMs(int ms) noexcept { latency_.store(static_cast<std::int16_t>(ms), std::memory_order_relaxed); }

    // ---- any thread ----
    int portMode(int port) const noexcept
    {
        return valid(port) ? mode_[static_cast<std::size_t>(port)].load(std::memory_order_relaxed) : 0;
    }
    int portOffsetMs(int port) const noexcept
    {
        return valid(port) ? offset_[static_cast<std::size_t>(port)].load(std::memory_order_relaxed) : 0;
    }
    int outputLatencyMs() const noexcept { return latency_.load(std::memory_order_relaxed); }
    const CcTable* ccTable() const noexcept { return cc_.load(std::memory_order_acquire); }
    std::uint32_t outputMask() const noexcept { return outputs_.load(std::memory_order_relaxed); }
    std::uint32_t inputMask() const noexcept { return inputs_.load(std::memory_order_relaxed); }
    std::uint32_t generation(int port) const noexcept
    {
        return valid(port) ? generation_[static_cast<std::size_t>(port)].load(std::memory_order_acquire) : 0;
    }
    std::uint64_t inDropped() const noexcept { return inDropped_.load(std::memory_order_relaxed); }
    std::uint64_t outDropped() const noexcept { return outDropped_.load(std::memory_order_relaxed); }
    std::uint64_t uiDropped() const noexcept { return uiDropped_.load(std::memory_order_relaxed); }
    std::uint64_t forwardDropped() const noexcept { return forwardDropped_.load(std::memory_order_relaxed); }

private:
    static constexpr bool valid(int port) noexcept { return port >= 0 && port < kMaxPorts; }
    static bool count(std::atomic<std::uint64_t>& c) noexcept
    {
        c.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::array<SpscQueue<MidiEvent, kInQueue>, kMaxPorts> in_ {};
    std::array<SpscQueue<MidiEvent, kInQueue>, kMaxPorts> forward_ {};
    SpscQueue<MidiOutEvent, kOutQueue> out_ {};
    SpscQueue<UiEvent, kUiQueue> ui_ {};
    std::atomic<std::uint32_t> seq_ {0};
    std::atomic<std::int64_t> frame_ {0}, hostNs_ {0};
    std::atomic<double> nsPerFrame_ {1.0e9 / 44100.0};
    std::atomic<bool> valid_ {false};
    std::atomic<const CcTable*> cc_ {nullptr};
    std::atomic<std::uint32_t> outputs_ {0}, inputs_ {0};
    std::array<std::atomic<std::uint32_t>, kMaxPorts> generation_ {};
    std::array<std::atomic<std::int8_t>, kMaxPorts> mode_ {};
    std::array<std::atomic<std::int16_t>, kMaxPorts> offset_ {};
    std::atomic<std::int16_t> latency_ {12};
    std::atomic<std::uint64_t> inDropped_ {0}, outDropped_ {0}, uiDropped_ {0}, forwardDropped_ {0};
};

// The process's live hub (the engine the app runs). claimLiveHub succeeds only when none is claimed (first wins: the
// app's engine is created before any offline-render engine); releaseLiveHub only releases its own claim.
MidiHub* liveHub() noexcept;
bool claimLiveHub(MidiHub*) noexcept;
void releaseLiveHub(MidiHub*) noexcept;

// Monotonic clock (CLOCK_MONOTONIC, the clock of AudioIO::hostTimeNs and of the port threads' stamps).
std::int64_t monotonicNs() noexcept;
} // namespace core::midi
