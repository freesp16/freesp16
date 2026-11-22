// Helpers of the scene tests: a drift-free block clock that fills BlockContexts the way the transport does (PPQ from a
// block counter since the last tempo change, start edges, stop), and fakes of the switcher's side-effect targets.
#pragma once

#include "engine/EngineApi.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"

#include <cstdint>
#include <vector>

namespace scenetest
{
struct Clock
{
    double sampleRate = 44100.0, bpm = 120.0;
    bool playing = false, startEdge = false, preCount = false;
    std::int64_t block = 0;           // blocks rendered so far (the absolute frame of block b, frame f = b * 64 + f)
    std::int64_t anchorBlock = 0;     // block of the last tempo change / start
    double anchorPpq = 0.0, stoppedPpq = 0.0;

    double ppqPerFrame() const noexcept { return bpm / (60.0 * sampleRate); }
    double ppqAt(std::int64_t b) const noexcept
    {
        return anchorPpq + static_cast<double>(b - anchorBlock) * engine::kBlockFrames * ppqPerFrame();
    }
    void play(double from = 0.0)
    {
        playing = startEdge = true;
        anchorBlock = block;
        anchorPpq = from;
    }
    void stop()
    {
        stoppedPpq = ppqAt(block);
        playing = false;
    }
    void setTempo(double b)
    {
        if (playing)
        {
            anchorPpq = ppqAt(block);
            anchorBlock = block;
        }
        bpm = b;
    }
    // The next block's context (advances the clock).
    engine::BlockContext next()
    {
        engine::BlockContext c {};
        c.sampleRate = sampleRate;
        c.frames = engine::kBlockFrames;
        c.bpm = bpm;
        c.playing = playing;
        c.startedThisBlock = startEdge;
        c.blockIndex = static_cast<std::uint64_t>(block);
        c.ppqStart = playing ? ppqAt(block) : stoppedPpq;
        c.ppqEnd = playing ? ppqAt(block + 1) : stoppedPpq;
        startEdge = false;
        ++block;
        return c;
    }
};

struct FakeTransport
{
    std::vector<engine::TransportCmd> commands;
    void command(const engine::TransportCmd& c) { commands.push_back(c); }
};

struct FakeMixer
{
    struct Masks
    {
        std::uint16_t mute, solo;
        int frame;
    };
    struct SceneSet
    {
        int scene, frame;
    };
    std::vector<Masks> masks;
    std::vector<SceneSet> scenes;
    void setPatternMuteSolo(std::uint16_t m, std::uint16_t s, int frame) { masks.push_back({m, s, frame}); }
    void setScene(int scene, int frame) { scenes.push_back({scene, frame}); }
};

struct FakeEvents
{
    std::vector<engine::EngineEvent> events;
    bool pushEvent(const engine::EngineEvent& e)
    {
        events.push_back(e);
        return true;
    }
};

// A switcher driven by a Clock, recording where switches happened (absolute frames).
struct Rig
{
    engine::scene::ScenePatternSwitcher sw;
    Clock clock;
    engine::stages::BlockBuffers::Timing timing {};
    FakeTransport transport;
    FakeMixer mixer;
    FakeEvents events;
    std::int64_t lastSwitchFrame = -1; // absolute frame of the last switch
    engine::scene::SwitchResult last {};

    engine::scene::SwitchResult step()
    {
        const auto b = clock.block;
        timing.preCount = clock.preCount;
        last = sw.process(clock.next(), timing, transport, mixer, events);
        if (last.switched)
            lastSwitchFrame = b * engine::kBlockFrames + last.frame;
        return last;
    }
    // Runs blocks until the block that starts at or after `ppq` has run.
    void runUntilPpq(double ppq)
    {
        while (clock.ppqAt(clock.block) < ppq)
            step();
    }
    // Runs until a switch happened (at most `maxBlocks`); true when one did.
    bool runUntilSwitch(int maxBlocks = 100000)
    {
        for (int i = 0; i < maxBlocks; ++i)
            if (step().switched)
                return true;
        return false;
    }
};
} // namespace scenetest
