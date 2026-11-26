// A ui::m5::SamplingHost for the sampling-screen tests: a real CaptureHub (ring + status) the test writes into, and a
// log of what the screens asked for. Generated data only.
#pragma once

#include "ui/screens/sampling/SamplingHost.h"

#include <cmath>
#include <string>
#include <vector>

namespace m5test
{
class FakeSamplingHost final : public ui::m5::SamplingHost
{
public:
    explicit FakeSamplingHost(std::int64_t ringFrames = 44100 * 40)
    {
        hub_ = new core::sampling::CaptureHub();
        hub_->ring.allocate(ringFrames);
        install(this);
    }
    ~FakeSamplingHost() override
    {
        install(nullptr);
        hub_->release();
    }
    core::sampling::CaptureHub& hubRef() { return *hub_; }
    const core::sampling::CaptureHub* hub() const override { return withRing ? hub_ : nullptr; }

    // Writes `frames` of a square wave of amplitude amp(i) per `per`-frame column into the ring.
    template <typename F> void writeColumns(int columns, double per, F amp)
    {
        std::vector<float> buf;
        std::int64_t written = hub_->ring.head();
        const std::int64_t start = written;
        for (int c = 0; c < columns; ++c)
        {
            const auto end = start + static_cast<std::int64_t>(std::llround((c + 1) * per));
            const float a = amp(c);
            buf.clear();
            for (auto f = written; f < end; ++f)
                buf.push_back((f & 1) != 0 ? a : -a);
            for (std::size_t off = 0; off < buf.size(); off += 4096)
            {
                const int n = static_cast<int>(std::min<std::size_t>(4096, buf.size() - off));
                hub_->ring.write(buf.data() + off, nullptr, n);
            }
            written = end;
        }
    }
    void writeSilence(std::int64_t frames)
    {
        std::vector<float> z(4096, 0.0f);
        while (frames > 0)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(4096, frames));
            hub_->ring.write(z.data(), nullptr, n);
            frames -= n;
        }
    }

    bool record(int scene, int track) override
    {
        calls.push_back("record " + std::to_string(scene) + " " + std::to_string(track));
        auto& s = hub_->status;
        s.takeScene = scene;
        s.takeTrack = track;
        s.takeSeq = s.takeSeq.load() + 1;
        s.state = static_cast<int>(core::sampling::RecState::Standby);
        return true;
    }
    void stop(int scene, int track) override
    {
        calls.push_back("stop " + std::to_string(scene) + " " + std::to_string(track));
        hub_->status.state = static_cast<int>(core::sampling::RecState::Idle);
    }
    void drop(int scene, int track) override
    {
        calls.push_back("drop " + std::to_string(scene) + " " + std::to_string(track));
        hub_->status.state = static_cast<int>(core::sampling::RecState::Cancelled);
    }
    bool unsaved(int, int track) const override { return unsavedTrack == track; }
    std::string proposedName(int, int) const override { return "Beat_120bpm_2bar_01"; }
    void saveSelection(int, int track, std::int64_t start, std::int64_t end, const std::string& name, bool replace,
                       ui::m5::HostDone done) override
    {
        calls.push_back("save " + std::to_string(track) + " " + std::to_string(start) + " " + std::to_string(end) + " " +
                        name + (replace ? " replace" : " keep"));
        unsavedTrack = -1;
        if (done)
            done(true, {});
    }
    void discard(int, int track) override
    {
        calls.push_back("discard " + std::to_string(track));
        unsavedTrack = -1;
    }
    void process(int, int track, ui::m5::TakeProcess p, std::int64_t, std::int64_t, ui::m5::HostDone done) override
    {
        calls.push_back("process " + std::to_string(track) + " " + std::to_string(static_cast<int>(p)));
        if (done)
            done(true, "done");
    }
    bool tempo(int, int, double& bars, double& bpm) const override
    {
        bars = tempoBars;
        bpm = tempoBpm;
        return tempoBars > 0;
    }
    void setTempo(int, int, double bars, double bpm) override
    {
        tempoBars = bars;
        tempoBpm = bpm;
        calls.push_back("tempo");
    }
    bool liveArm(int scene, int track) override
    {
        calls.push_back("arm " + std::to_string(scene) + " " + std::to_string(track));
        hub_->status.liveState = static_cast<int>(core::sampling::LiveState::Armed);
        return true;
    }
    void liveDisarm(int, int track) override
    {
        calls.push_back("disarm " + std::to_string(track));
        hub_->status.liveState = static_cast<int>(core::sampling::LiveState::Idle);
    }
    void setNudge(std::int32_t frames) override
    {
        nudge = frames;
        hub_->status.liveNudge = frames;
    }
    bool liveUnsaved() const override { return liveTake >= 0 && liveDirty; }
    int liveScene() const override { return liveTakeScene; }
    int liveTrack() const override { return liveTake; }
    void saveLive(int to, ui::m5::HostDone done) override
    {
        calls.push_back("saveLive " + std::to_string(to));
        liveDirty = false;
        if (done)
            done(true, {});
    }
    bool unloadLive() override
    {
        calls.push_back("unload " + std::to_string(liveTake));
        liveTake = -1;
        return true;
    }
    std::uint32_t revision() const override { return rev; }

    bool called(const std::string& prefix) const
    {
        for (const auto& c : calls)
            if (c.rfind(prefix, 0) == 0)
                return true;
        return false;
    }

    std::vector<std::string> calls;
    int unsavedTrack = -1, liveTake = -1, liveTakeScene = 0;
    bool liveDirty = true, withRing = true;
    double tempoBars = 2, tempoBpm = 120;
    std::int32_t nudge = 0;
    std::uint32_t rev = 1;

private:
    core::sampling::CaptureHub* hub_;
};
} // namespace m5test
