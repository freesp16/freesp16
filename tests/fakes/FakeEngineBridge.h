// FakeEngineBridge: an engine::EngineBridge that records what the UI/command side posts and serves feeds and events a
// test sets, so ui/services code is tested without an engine.
#pragma once

#include "engine/EngineApi.h"

#include <deque>
#include <vector>

namespace fakes
{
class FakeEngineBridge final : public engine::EngineBridge
{
public:
    bool post(const engine::EngineMessage& m) override;
    void apply(const core::ChangeSet&, const engine::model::Project&) override { ++applied; }
    const engine::PlayheadFeed& playhead() const override { return playheadFeed; }
    const engine::MeterFeed& meters() const override { return meterFeed; }
    void retirePending() override { ++retireCalls; }
    bool popEvent(engine::EngineEvent& out) override;

    // Test side.
    std::vector<engine::EngineMessage> posted;
    std::deque<engine::EngineEvent> events;
    engine::PlayheadFeed playheadFeed {};
    engine::MeterFeed meterFeed {};
    int applied = 0, retireCalls = 0;
    bool accept = true; // post() result
};
} // namespace fakes
