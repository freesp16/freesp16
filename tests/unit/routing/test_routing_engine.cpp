// The routing inside engine::Engine ("track 1 → OUT3-4; MAIN still carries it", "the 8CH one only on its pair"):
// routing.* parameters through the bridge, the device's port presence from AudioIO, the Thru BYPASS on the FX slot
// bank, the parameter catalogue. Rendered with OfflineDriver (real-time marked: any allocation aborts under
// APP_RT_CHECKS).
#include "tests/unit/engine/EngineTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "engine/fx/FxSlotState.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

constexpr int kPorts = static_cast<int>(engine::OutPort::Count);

// Renders `frames` frames of every logical port (2 × 6 planar channels); ports beyond `present` are absent (null).
struct Ports
{
    std::array<std::vector<float>, 2 * kPorts> ch;
    float peak(engine::OutPort p) const
    {
        float m = 0.0f;
        for (int s = 0; s < 2; ++s)
            for (float x : ch[static_cast<std::size_t>(2 * static_cast<int>(p) + s)])
                m = std::max(m, std::fabs(x));
        return m;
    }
};
Ports render(engine::Engine& e, int frames, int presentPairs = kPorts)
{
    Ports p;
    std::array<float*, 2 * kPorts> out {};
    for (int c = 0; c < 2 * presentPairs; ++c)
    {
        p.ch[static_cast<std::size_t>(c)].assign(static_cast<std::size_t>(frames), 0.0f);
        out[static_cast<std::size_t>(c)] = p.ch[static_cast<std::size_t>(c)].data();
    }
    engine::OfflineDriver d;
    d.periodFrames = 256;
    d.run(e, frames, std::span<float* const>(out.data(), static_cast<std::size_t>(2 * presentPairs)));
    return p;
}
} // namespace

TEST_CASE("routing.engine: track 1 → OUT 3-4 keeps MAIN in M + 6CH; 8CH plays it only on OUT 3-4", "[routing][engine]")
{
    auto e = enginetest::make(); // the 440 Hz test tone on track 1 (−12 dBFS)
    auto& b = e->bridge();
    render(*e, 1024);
    auto before = render(*e, 2048);
    CHECK(before.peak(engine::OutPort::Main) > 0.24f);
    CHECK(before.peak(engine::OutPort::Out34) == 0.0f);
    REQUIRE(b.post(engine::ParamChange {id("routing.outAssign"), 0, 2.0f, 0})); // scene 1 track 1 → OUT3-4
    render(*e, 1024);
    auto m6 = render(*e, 2048);
    CHECK(m6.peak(engine::OutPort::Main) > 0.24f);
    CHECK(m6.peak(engine::OutPort::Out34) > 0.24f);
    CHECK(m6.peak(engine::OutPort::Out56) == 0.0f);
    CHECK(m6.peak(engine::OutPort::Phones) > 0.12f); // cue MASTER at HEADPHONES 100 (−6 dB)
    REQUIRE(b.post(engine::ParamChange {id("routing.assignMode"), 0, 1.0f, 0})); // 8CH
    render(*e, 1024);
    auto c8 = render(*e, 2048);
    CHECK(c8.peak(engine::OutPort::Main) == 0.0f);
    CHECK(c8.peak(engine::OutPort::Out34) > 0.24f);
    // The same on a stereo device: OUT 3-4 is absent, the track folds into MAIN (MIX-032).
    auto stereo = render(*e, 2048, 1);
    CHECK(stereo.peak(engine::OutPort::Main) > 0.24f);
    // The resample tap is the master (post master fader, pre trim) whatever the mode.
    CHECK(e->routing().resampleTap(0) != nullptr);
}

TEST_CASE("routing.engine: a mode change dips MAIN over 128 + 128 samples, nothing longer", "[routing][engine]")
{
    auto e = enginetest::make();
    auto& b = e->bridge();
    render(*e, 2048);
    REQUIRE(b.post(engine::ParamChange {id("routing.outAssign"), 0, 1.0f, 0})); // OUT1-2: master only in M + 6CH
    render(*e, 1024);
    REQUIRE(b.post(engine::ParamChange {id("routing.assignMode"), 0, 1.0f, 0}));
    REQUIRE(b.post(engine::ParamChange {id("routing.assignMode"), 0, 0.0f, 0})); // the same block: re-selected
    auto p = render(*e, 1024);
    CHECK(p.peak(engine::OutPort::Main) > 0.24f); // the dip turned round: no stuck silence
    CHECK_FALSE(e->routing().router().fading());
}

TEST_CASE("routing.engine: the routing parameters are bound and the catalogue matches the spec lists", "[routing][params]")
{
    auto e = enginetest::make();
    for (const char* p : {"routing.assignMode", "routing.outAssign", "routing.headphoneCue", "routing.monitorLevel",
                          "routing.monitor", "routing.thruBypass"})
    {
        INFO(p);
        CHECK(id(p).valid());
        CHECK(e->bridge().post(engine::ParamChange {id(p), 0, 0.0f, 0}));
    }
    render(*e, 256);
    const auto& r = core::builtinParams();
    CHECK(r.errors().empty());
    const auto* mode = r.find("routing.assignMode");
    REQUIRE(mode != nullptr);
    CHECK(mode->scope == core::ParamScope::Scene);
    CHECK(mode->enumLabels.size() == 2);
    CHECK(mode->enumLabels[0] == "M + 6CH");
    const auto* out = r.find("routing.outAssign");
    REQUIRE(out != nullptr);
    CHECK(out->scope == core::ParamScope::Track);
    CHECK(out->maxValue == 12.0f);
    CHECK(out->enumLabels[0] == "--");
    CHECK(out->enumLabels[2] == "OUT3-4");
    const auto* cue = r.find("routing.headphoneCue");
    REQUIRE(cue != nullptr);
    CHECK(cue->enumLabels[0] == "MASTER");
    CHECK(cue->enumLabels[12] == "8");
    const auto* level = r.find("routing.monitorLevel");
    REQUIRE(level != nullptr);
    CHECK(level->defaultValue == 100.0f);
    CHECK(level->scope == core::ParamScope::Project);
}

TEST_CASE("routing.engine: Thru BYPASS bypasses both inserts of a Thru track and gives back the stored state",
          "[routing][engine]")
{
    engine::EngineOptions o;
    o.testTone = false;
    auto e = std::make_unique<engine::Engine>(enginetest::fullSetup(), o);
    e->prepare(44100.0, 512);
    auto& b = e->bridge();
    const int s0 = engine::fx::insertSlot(2, 0), s1 = engine::fx::insertSlot(2, 1);
    REQUIRE(b.post(engine::ParamChange {id("routing.thruBypass"), 2, 1.0f, 0})); // scene 1 track 3
    render(*e, 256);
    CHECK_FALSE(e->fxBank().slot(s0).bypassed()); // a Sample track: the switch only acts on Thru tracks
    REQUIRE(b.post(engine::ParamChange {id("track.mode"), 2, 2.0f, 0})); // THRU
    render(*e, 256);
    CHECK(e->fxBank().slot(s0).bypassed());
    CHECK(e->fxBank().slot(s1).bypassed());
    CHECK_FALSE(e->fxBank().slot(engine::fx::insertSlot(3, 0)).bypassed());
    REQUIRE(b.post(engine::ParamChange {id("fx1.bypass"), 2, 0.0f, 0})); // the stored FX1 bypass: still held by Thru
    render(*e, 256);
    CHECK(e->fxBank().slot(s0).bypassed());
    REQUIRE(b.post(engine::ParamChange {id("fx2.bypass"), 2, 1.0f, 0})); // stored FX2 bypass on
    REQUIRE(b.post(engine::ParamChange {id("routing.thruBypass"), 2, 0.0f, 0}));
    render(*e, 256);
    CHECK_FALSE(e->fxBank().slot(s0).bypassed()); // back to the stored states
    CHECK(e->fxBank().slot(s1).bypassed());
}
