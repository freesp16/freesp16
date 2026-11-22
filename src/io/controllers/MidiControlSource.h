// One MIDI pad controller on one input port (control-surface.md §2.4 UI-130…136, UI-118): a ControlSource whose events
// come from the port's reader thread through a compiled profile (profiles/Mapper.h) and a LampSink that sends
// the profile's lamp messages (profiles/LampEncoder.h).
//
//   reader thread  onMidi(): MMC / realtime / channel message → table lookup → ControlEvents into an SPSC queue; a pad
//                  hit also goes straight into the engine's TriggerSink (UI-118). Consumed messages (UI-136) are the
//                  ones a binding matches. No allocation, no lock, no logging. While MIDI learn is armed the next
//                  message is captured instead (consumed, nothing mapped).
//   UI thread      poll() drains the queue (≤ 512 events between two frames, more are dropped and counted);
//                  setLamp()/flush() diff the lamps and send the changed ones, ≤ 60 Hz (UI-135), through `send`.
// The constructor takes no EngineBridge&: the source factory hands sources an engine::TriggerSink*, which is the
// UI-118 path; the port is the MidiService input slot.
#pragma once

#include "core/util/SpscQueue.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/profiles/LampEncoder.h"
#include "io/controllers/profiles/Mapper.h"
#include "io/controllers/profiles/Profile.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace io::controllers
{
// A message captured by MIDI learn: status byte (with channel) and the first data byte.
struct LearnedMessage
{
    std::uint8_t status = 0, number = 0, value = 0;
    bool valid() const noexcept { return status != 0; }
};

class MidiControlSource final : public ControlSource, public LampSink
{
public:
    static constexpr std::size_t kQueue = 512;

    MidiControlSource(const LoadedProfile& profile, int inSlot, std::string portName, engine::TriggerSink* triggers);
    std::string_view id() const override { return id_; } // "midi:<port>"
    void poll(std::vector<ControlEvent>& out) override;
    void setLamp(Control, std::uint32_t rgb, std::uint8_t level) override;
    void flush() override;
    void flushAt(double nowMs); // flush() with an explicit clock (tests)

    // Reader thread. True = consumed (the message belongs to the controller).
    bool onMidi(const std::uint8_t* msg, int len, double hostMs) noexcept;
    // Reader thread: raw bytes as read, for MMC (SysEx is skipped by the port's parser).
    void onRawBytes(const std::uint8_t* bytes, int n, double hostMs) noexcept;

    // UI thread.
    void setSend(LampEncoder::Send send) { send_ = std::move(send); encoder_.resend(); }
    bool hasLamps() const noexcept { return ! encoder_.empty(); }
    void setResponse(const PadResponse& r) noexcept { mapper_.setResponse(r); }
    void setTrim(int pad0, int trim) noexcept { mapper_.setTrim(pad0, trim); }
    void armLearn() noexcept;
    void cancelLearn() noexcept { learnArmed_.store(false, std::memory_order_release); }
    bool learning() const noexcept { return learnArmed_.load(std::memory_order_acquire); }
    bool takeLearned(LearnedMessage& out) noexcept; // the captured message, once

    const std::string& profileId() const noexcept { return profileId_; }
    const std::string& portName() const noexcept { return port_; }
    int slot() const noexcept { return slot_; }
    int lastVelocity() const noexcept { return mapper_.lastVelocity(); }
    const ControllerMapper& mapper() const noexcept { return mapper_; }
    const LampEncoder& lamps() const noexcept { return encoder_; }

    struct Stats
    {
        std::uint64_t messages = 0, consumed = 0, events = 0, triggers = 0, dropped = 0, lampsSent = 0;
    };
    Stats stats() const noexcept;

private:
    std::string port_, id_, profileId_;
    int slot_;
    engine::TriggerSink* triggers_;
    ControllerMapper mapper_;
    LampEncoder encoder_;
    LampEncoder::Send send_;
    core::SpscQueue<ControlEvent, kQueue> queue_ {};
    // MMC scanner (reader thread): F0 7F dd 06 cc F7.
    std::uint8_t sysex_[6] {};
    int sysexLen_ = -1;
    std::atomic<bool> learnArmed_ {false}, learnReady_ {false};
    LearnedMessage learned_ {};
    std::atomic<std::uint64_t> messages_ {0}, consumed_ {0}, events_ {0}, triggered_ {0}, dropped_ {0};
    std::uint64_t lampsSent_ = 0;
};
} // namespace io::controllers
