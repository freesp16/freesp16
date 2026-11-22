// FakeControlSource: an io::ControlSource fed by the test (scripted ControlEvents); pad presses also go to the
// TriggerSink like a real source (UI-118), so the latency path is testable without hardware.
#pragma once

#include "io/controllers/ControlSurface.h"

#include <deque>
#include <string>
#include <vector>

namespace fakes
{
class FakeControlSource final : public io::ControlSource
{
public:
    explicit FakeControlSource(engine::TriggerSink* triggers = nullptr, std::string id = "fake") : triggers_(triggers), id_(std::move(id)) {}
    std::string_view id() const override { return id_; }
    bool start(std::string& info) override
    {
        info = "fake: scripted events";
        return started = true;
    }
    void stop() override { started = false; }
    void poll(std::vector<io::ControlEvent>& out) override
    {
        out.insert(out.end(), queue_.begin(), queue_.end());
        queue_.clear();
        ++polls;
    }

    // Test side: queue an event (as the reader thread would).
    void emit(const io::ControlEvent& e)
    {
        if (e.kind == io::ControlEvent::Kind::Press && io::isPad(e.control) && triggers_ != nullptr)
        {
            const auto pad = static_cast<std::uint8_t>(io::indexOf(e.control) - 1);
            triggers_->push({pad, static_cast<std::uint8_t>(e.value), 60, static_cast<std::int8_t>(pad)});
        }
        queue_.push_back(e);
    }

    bool started = false;
    int polls = 0;

private:
    engine::TriggerSink* triggers_;
    std::string id_;
    std::deque<io::ControlEvent> queue_;
};

// FakeLampSink: records the last colour/level per control.
class FakeLampSink final : public io::LampSink
{
public:
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override { lamps.push_back({c, rgb, level}); }
    void flush() override { ++flushes; }
    struct Lamp
    {
        io::Control control;
        std::uint32_t rgb;
        std::uint8_t level;
    };
    std::vector<Lamp> lamps;
    int flushes = 0;
};
} // namespace fakes
