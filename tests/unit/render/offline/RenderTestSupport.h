// Generated projects and samples for the offline render tests and bench_render_offline (never device data): a kick-like
// decaying sine and a hat-like noise burst on tracks 1 / 2 of scene 1, pattern 1 with hits on both, and a long ringing
// sample for the tail tests. The project forwarding is the app's (app/ProjectFeed.cpp, compiled into the test
// executable once by FeedSupport.cpp).
#pragma once

#include "app/ProjectFeed.h"
#include "core/audio/SampleBufferStorage.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "services/render/OfflineEngine.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace rendertest
{
struct BufferDeleter
{
    void operator()(core::audio::SampleBuffer* b) const noexcept { core::audio::freeSampleBuffer(b); }
};
using Buffer = std::unique_ptr<core::audio::SampleBuffer, BufferDeleter>;

inline Buffer makeBuffer(std::int64_t frames, double rate, std::uint64_t id, float (*shape)(std::int64_t, double))
{
    Buffer b(core::audio::allocateSampleBuffer(frames, 1, rate));
    b->contentId = id;
    auto* x = core::audio::writableChannel(b.get(), 0);
    for (std::int64_t i = 0; i < frames; ++i)
        x[i] = shape(i, rate);
    return b;
}

inline float kick(std::int64_t i, double rate)
{
    const double t = static_cast<double>(i) / rate;
    return static_cast<float>(0.8 * std::exp(-t * 12.0) * std::sin(2 * 3.141592653589793 * (55.0 + 80.0 * std::exp(-t * 30.0)) * t));
}

inline float hat(std::int64_t i, double)
{
    std::uint32_t x = static_cast<std::uint32_t>(i) * 2654435761u + 12345u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return static_cast<float>(0.4 * std::exp(-static_cast<double>(i) / 800.0) * (static_cast<double>(x) / 4294967295.0 * 2 - 1));
}

inline float ring(std::int64_t i, double rate)
{
    const double t = static_cast<double>(i) / rate;
    return static_cast<float>(0.5 * std::exp(-t * 3.0) * std::sin(2 * 3.141592653589793 * 330.0 * t));
}

struct Fixture
{
    std::unique_ptr<core::model::Project> project;
    std::vector<Buffer> buffers; // destroyed after every engine and snapshot that used them
    services::render::SampleSnapshot samples;
    double rate = 44100;

    Fixture() = default;
    Fixture(Fixture&&) = default;
    ~Fixture() { samples = {}; } // the snapshot's references go before the buffers
};

inline void hit(core::model::Pattern& p, int step, int track, int velocity = 110)
{
    auto& c = p.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
    c.trigger = 0;
    c.value2 = static_cast<std::int16_t>(velocity);
}

// Scene 1, tempo 120: track 1 kick on 1/5/9/13, track 2 hat on 3/7/11/15 (pattern 1, 16 steps); pattern 2 (8 steps):
// kick on 1 and 5. `ringAtEnd`: track 3 plays a 1 s ringing tone on step 16 of pattern 1 (a tail past the end).
inline Fixture makeFixture(bool ringAtEnd = false, double rate = 44100)
{
    Fixture f;
    f.rate = rate;
    f.project = std::make_unique<core::model::Project>(core::model::makeDefaultProject());
    auto& p = *f.project;
    p.settings.tempo = 120.0;
    auto& sc = p.scenes[0];
    sc.tracks[0].playback.sample.id = "kick";
    sc.tracks[1].playback.sample.id = "hat";
    for (const int s : {0, 4, 8, 12})
        hit(sc.patterns[0], s, 0);
    for (const int s : {2, 6, 10, 14})
        hit(sc.patterns[0], s, 1, 90);
    sc.patterns[1].length = 8;
    hit(sc.patterns[1], 0, 0);
    hit(sc.patterns[1], 4, 0);
    f.buffers.push_back(makeBuffer(static_cast<std::int64_t>(0.3 * rate), rate, 101, &kick));
    f.buffers.push_back(makeBuffer(4000, rate, 102, &hat));
    f.samples.set(0, 0, f.buffers[0].get());
    f.samples.set(0, 1, f.buffers[1].get());
    sc.tracks[0].playback.length = f.buffers[0]->frames; // LENGTH = the whole sample (as LIB-012's load sets it)
    sc.tracks[1].playback.length = f.buffers[1]->frames;
    if (ringAtEnd)
    {
        sc.tracks[2].playback.sample.id = "ring";
        hit(sc.patterns[0], 15, 2);
        f.buffers.push_back(makeBuffer(static_cast<std::int64_t>(1.0 * rate), rate, 103, &ring));
        f.samples.set(0, 2, f.buffers[2].get());
        sc.tracks[2].playback.length = f.buffers[2]->frames;
    }
    return f;
}

inline double peak(const std::vector<float>& v, std::size_t from = 0, std::size_t to = ~std::size_t {0})
{
    double m = 0;
    for (std::size_t i = from; i < std::min(to, v.size()); ++i)
        m = std::max(m, static_cast<double>(std::fabs(v[i])));
    return m;
}
} // namespace rendertest
