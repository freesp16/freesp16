// Helpers of the arrangement tests: a transport model (one tempo per block, SetTempo from the next block start, Stop to
// PPQ 0 at once — engine/transport/Transport.h), the song rig (SectionSequencer + ScenePatternSwitcher in step-5 order)
// and an independent model of where a song's row changes land (block by block, the boundary contract's frame rule).
#pragma once

#include "engine/scene/SectionSequencer.h"
#include "tests/unit/scene/SceneTestUtil.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <ostream>
#include <vector>

namespace arrtest
{
namespace es = engine::scene;

struct SongClock
{
    double rate = 44100.0, tempo = 120.0;
    bool playing = false, wasPlaying = false;
    double anchor = 0.0;
    std::int64_t frames = 0, block = 0;
    std::vector<engine::TransportCmd> commands;

    double ppf() const noexcept { return tempo / (60.0 * rate); }
    double pos() const noexcept { return anchor + static_cast<double>(frames) * ppf(); }
    double bpm() const noexcept { return tempo; }
    void command(const engine::TransportCmd& c)
    {
        commands.push_back(c);
        switch (c.kind)
        {
            case engine::TransportCmd::SetTempo:
                anchor = pos();
                frames = 0;
                tempo = c.value;
                break;
            case engine::TransportCmd::Stop:
                if (c.value < 0.5) // pause
                {
                    anchor = pos();
                    frames = 0;
                }
                else
                {
                    anchor = 0.0;
                    frames = 0;
                }
                playing = false;
                break;
            case engine::TransportCmd::Play:
                anchor = 0.0;
                frames = 0;
                playing = true;
                break;
            case engine::TransportCmd::Continue: playing = true; break;
            case engine::TransportCmd::Locate:
                anchor = c.value;
                frames = 0;
                break;
            default: break;
        }
    }
    int stops() const
    {
        int n = 0;
        for (const auto& c : commands)
            n += c.kind == engine::TransportCmd::Stop ? 1 : 0;
        return n;
    }
    engine::BlockContext next()
    {
        engine::BlockContext c {};
        c.sampleRate = rate;
        c.frames = engine::kBlockFrames;
        c.ppqStart = pos();
        if (playing)
            frames += engine::kBlockFrames;
        c.ppqEnd = pos();
        c.bpm = tempo;
        c.playing = playing;
        c.startedThisBlock = playing && ! wasPlaying;
        c.stoppedThisBlock = ! playing && wasPlaying;
        c.blockIndex = static_cast<std::uint64_t>(block);
        wasPlaying = playing;
        ++block;
        return c;
    }
};

struct Switch
{
    std::int64_t frame;
    int scene, pattern;
    friend bool operator== (const Switch&, const Switch&) = default;
    friend std::ostream& operator<< (std::ostream& o, const Switch& s)
    {
        return o << "{" << s.frame << " " << s.scene << "." << s.pattern << "}";
    }
};

struct SongRig
{
    es::ScenePatternSwitcher sw;
    es::SectionSequencer seq;
    SongClock clock;
    engine::stages::BlockBuffers::Timing timing {};
    scenetest::FakeMixer mixer;
    scenetest::FakeEvents events;
    std::unique_ptr<es::ArrangementPlan> plan = std::make_unique<es::ArrangementPlan>();
    const es::ArrangementPlan* installed = nullptr;
    std::vector<Switch> switches;
    // Song position at the start of every row (SPP out): (row, songPositionAt(the row's first PPQ)).
    std::vector<std::pair<int, std::int64_t>> rowStarts;
    std::uint32_t seenChanges = 0;
    std::int64_t origin = 0; // frames of `switches` count from here (playFromTop sets it to the PLAY block)

    SongRig()
    {
        installed = plan.get();
        seq.setSongMode(true);
    }
    void step()
    {
        const auto b = clock.block;
        const engine::BlockContext ctx = clock.next();
        seq.process(ctx, timing, sw, clock, installed);
        const double before = clock.bpm();
        const auto r = sw.process(ctx, timing, clock, mixer, events);
        seq.afterSwitch(ctx, timing, clock, before);
        if (r.switched)
            switches.push_back({b * engine::kBlockFrames + r.frame - origin, timing.scene, timing.pattern});
        if (seq.rowChanges() != seenChanges)
        {
            seenChanges = seq.rowChanges();
            const double at = timing.boundary.has ? timing.boundary.ppq : (ctx.playing ? timing.patternStartPpq : 0.0);
            rowStarts.emplace_back(seq.playing().row, seq.songPositionAt(at));
        }
    }
    void run(std::int64_t blocks)
    {
        for (std::int64_t i = 0; i < blocks; ++i)
            step();
    }
};

struct Row
{
    int scene, pattern, length, repeats;
    float bpm; // ≤ 0 = "--"
};

// Fills the plan (and the switcher's lengths) from rows; returns the start table in sixteenths.
inline void setSong(SongRig& rig, const std::vector<Row>& rows)
{
    auto& p = *rig.plan;
    p.count = static_cast<std::uint16_t>(rows.size());
    std::int64_t at = 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i];
        p.rows[i] = {static_cast<std::uint8_t>(r.scene), static_cast<std::uint8_t>(r.pattern),
                     static_cast<std::uint8_t>(r.repeats), r.bpm};
        p.startPpq16[i] = at;
        at += static_cast<std::int64_t>(r.repeats) * r.length;
        rig.sw.setPatternLength(r.scene, r.pattern, r.length);
    }
    p.startPpq16[rows.size()] = at;
}

// Where the rows of a song change (absolute frame, row index), modelled block by block: PPQ advances 64 frames ×
// tempo per block; a row starting at PPQ B lands on frame ⌊(B − p0) / ppf + 1e-6⌋ of the block with p0 ≤ B < p1; its
// fixed BPM governs from the next block. Rows after the first; `endFrame` = frame of the song's end boundary.
struct Expect
{
    std::vector<std::pair<std::int64_t, int>> rows;
    std::int64_t endFrame = -1;
};
inline Expect expectRows(const std::vector<Row>& rows, double startTempo, double rate = 44100.0)
{
    Expect e;
    std::vector<double> starts; // quarters
    double q = 0.0;
    for (const auto& r : rows)
    {
        starts.push_back(q);
        q += r.repeats * r.length * 0.25;
    }
    starts.push_back(q);
    double tempo = rows[0].bpm > 0 ? static_cast<double>(rows[0].bpm) : startTempo;
    double anchor = 0.0; // drift-free like the transport: PPQ = anchor + frames since the last tempo change × ppf
    std::int64_t anchorBlock = 0;
    std::size_t next = 1;
    for (std::int64_t block = 0; next <= rows.size() && block < 100000000; ++block)
    {
        const double ppf = tempo / (60.0 * rate);
        const double p0 = anchor + static_cast<double>((block - anchorBlock) * engine::kBlockFrames) * ppf;
        const double p1 = anchor + static_cast<double>((block + 1 - anchorBlock) * engine::kBlockFrames) * ppf;
        double pendingTempo = 0.0;
        while (next <= rows.size())
        {
            const double b = starts[next];
            const double f = (b - p0) / ppf + 1e-6;
            if (f >= engine::kBlockFrames)
                break;
            const std::int64_t frame = block * engine::kBlockFrames + static_cast<std::int64_t>(f < 0 ? 0 : f);
            if (next == rows.size())
                e.endFrame = frame;
            else
            {
                e.rows.emplace_back(frame, static_cast<int>(next));
                if (rows[next].bpm > 0)
                    pendingTempo = static_cast<double>(rows[next].bpm);
            }
            ++next;
        }
        if (pendingTempo > 0)
        {
            anchor = p1;
            anchorBlock = block + 1;
            tempo = pendingTempo;
        }
    }
    return e;
}
} // namespace arrtest
