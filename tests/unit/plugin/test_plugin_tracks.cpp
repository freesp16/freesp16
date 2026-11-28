// Plugin tracks: TrackMode::Plugin model + undo + codec round-trip, DrawCmd::Op::Blit raster & damage,
// Engine SwapSlot::TrackPlugin + TrackSource triggers/audio/retirement, and TRACK TYPE -> PLUGIN -> SELECT PLUGIN ->
// track.plugin custom body screen + 6-knob StripOwner flow.
#include "app/PluginAdapter.h"
#include "core/audio/PluginNode.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/fx/FxCommands.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "core/model/enums/EnumStrings.h"
#include "core/params/ParamRegistry.h"
#include "engine/Engine.h"
#include "services/plugin/ClapHost.h"
#include "services/render/QueuedBridge.h"
#include "services/store/NativeCodec.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/midi/MidiTestUtil.h"
#include "tests/unit/sequencer/SeqRig.h"
#include "tests/unit/store/StoreTestSupport.h"
#include "tests/unit/trigger/TriggerRig.h"
#include "ui/input/steps/PatternSync.h"
#include "ui/render/Damage.h"
#include "ui/render/DrawList.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/screens/plugin/PluginSlots.h"
#include "ui/shell/Shell.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
class MockPluginNode final : public core::audio::PluginAudioNode
{
public:
    explicit MockPluginNode(std::atomic<int>* destroyedCounter) noexcept : destroyed_(destroyedCounter) {}
    ~MockPluginNode() override
    {
        if (destroyed_ != nullptr)
            destroyed_->fetch_add(1, std::memory_order_relaxed);
    }

    void noteOn(int, int note, float velocity) noexcept override
    {
        lastNote = note;
        lastVel = velocity;
        sounding = true;
        ++noteOns;
    }
    void noteOff(int, int note) noexcept override
    {
        lastOffNote = note;
        if (note == lastNote)
            sounding = false;
        ++noteOffs;
    }
    void allNotesOff(int) noexcept override
    {
        sounding = false;
        ++allOffs;
    }
    bool process(const core::audio::PluginProcessContext& ctx, float* outL, float* outR) noexcept override
    {
        if (! sounding)
            return false;
        for (int i = 0; i < ctx.frames; ++i)
        {
            outL[i] = 0.25f;
            outR[i] = -0.25f;
        }
        return true;
    }
    bool active() const noexcept override { return sounding; }

    void resetEffect() noexcept override { ++resets; }
    int resets = 0;
    int lastNote = -1, lastOffNote = -1;
    float lastVel = 0.0f;
    int noteOns = 0, noteOffs = 0, allOffs = 0;
    bool sounding = false;

private:
    std::atomic<int>* destroyed_;
};

MockPluginNode& installPlugin(engine::Engine& e, std::uint8_t track)
{
    auto plugin = std::make_unique<MockPluginNode>(nullptr);
    REQUIRE(e.bridge().post(engine::ObjectSwap{{engine::SwapSlot::TrackPlugin, 0, track}, plugin.get()}));
    return *plugin.release();
}

void postTrackParam(engine::EngineBridge& bridge, std::string_view id, std::uint16_t track, float value)
{
    REQUIRE(bridge.post(engine::ParamChange{miditest::id(id), track, value, 0}));
}

class MockCustomScreenAccess final : public ui::plugin::PluginAccess
{
public:
    MockCustomScreenAccess()
    {
        catalogue_ = {{"com.test.anvil", "Anvil", "Test", "Anvil.clap", true}};
        pixels_.assign(static_cast<std::size_t>(ui::plugin::kScreenWidth) * ui::plugin::kScreenHeight, 0xFF14181Eu);
    }

    std::vector<ui::plugin::PluginEntry> catalogue() const override { return catalogue_; }
    bool hasLoaded(int scene, int track) const noexcept override { return scene == 1 && track == 1 && loaded_; }
    std::string_view loadedName(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("Anvil") : std::string_view{};
    }
    std::string_view loadedVendor(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("Test") : std::string_view{};
    }
    std::string_view loadedId(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("com.test.anvil") : std::string_view{};
    }
    std::string_view loadedPath(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("Anvil.clap") : std::string_view{};
    }
    bool selectPlugin(int scene, int track, std::string_view, std::string_view id) override
    {
        if (scene == 1 && track == 1 && id == "com.test.anvil")
        {
            loaded_ = true;
            ++changes_;
            return true;
        }
        return false;
    }
    void clearPlugin(int scene, int track) override
    {
        if (scene == 1 && track == 1)
        {
            loaded_ = false;
            ++changes_;
        }
    }
    bool bypass(int, int) const noexcept override { return bypassed_; }
    void setBypass(int, int, bool on) override
    {
        bypassed_ = on;
        ++changes_;
    }
    std::vector<ui::plugin::PluginPresetEntry> presets(int, int, std::optional<ui::fx::Slot>) const override
    {
        if (! loaded_)
            return {};
        std::vector<ui::plugin::PluginPresetEntry> entries;
        if (! savedName.empty())
            entries.push_back({savedName, savedName, true});
        entries.push_back({"fm-perc", "FM Perc"});
        entries.push_back({"kick", "Kick"});
        return entries;
    }
    std::string_view currentPreset(int, int, std::optional<ui::fx::Slot> = {}) const noexcept override
    {
        return loaded_ ? preset_ : std::string_view{};
    }
    bool loadPreset(int, int, const ui::plugin::PluginPresetEntry& preset, std::optional<ui::fx::Slot>) override
    {
        if (! loaded_)
            return false;
        preset_ = preset.key;
        ++changes_;
        return true;
    }
    std::string savedName;
    int writes = 0;
    core::Result<bool> savePreset(int, int, std::optional<ui::fx::Slot>, const std::string& name, bool replace) override
    {
        if (savedName == name && ! replace)
            return false;
        savedName = name;
        ++writes;
        return true;
    }

    bool readKnobs(int scene, int track, ui::plugin::PluginKnobBank& out,
                   std::optional<ui::fx::Slot> = {}) const noexcept override
    {
        out = {};
        if (! hasLoaded(scene, track))
        {
            out.tab.set("PLUGIN");
            return false;
        }
        out.tab.set("VCO");
        static constexpr std::array<std::string_view, 6> kLabels = {
            "VCO1 FREQ", "VCO1 EG", "VCO1 LVL", "VCO2 FREQ", "VCO2 EG", "FM AMT",
        };
        for (int i = 0; i < 6; ++i)
        {
            auto& k = out.columns[static_cast<std::size_t>(i)];
            k.active = true;
            k.paramId = static_cast<std::uint32_t>(i);
            k.norm = knobs_[static_cast<std::size_t>(i)];
            k.label.set(kLabels[static_cast<std::size_t>(i)]);
            k.valueText.format("%d%%", static_cast<int>(k.norm * 100.0f + 0.5f));
        }
        return true;
    }
    bool turnKnob(int scene, int track, int col0, int detents, bool fine,
                  std::optional<ui::fx::Slot> = {}) noexcept override
    {
        if (! hasLoaded(scene, track) || col0 < 0 || col0 >= 6)
            return false;
        const float step = fine ? 0.01f : 0.05f;
        auto& v = knobs_[static_cast<std::size_t>(col0)];
        v = std::clamp(v + static_cast<float>(detents) * step, 0.0f, 1.0f);
        ++changes_;
        return true;
    }
    bool pushKnob(int, int, int) noexcept override { return false; }
    int pageCount(int, int) const noexcept override { return 1; }
    int currentPage(int, int) const noexcept override { return 0; }
    void setPage(int, int, int) noexcept override {}
    bool hasCustomScreen(int scene, int track, std::optional<ui::fx::Slot> = {}) const noexcept override
    {
        return hasLoaded(scene, track);
    }
    ui::plugin::PluginScreenFrame screenRender(int scene, int track, std::uint32_t,
                                               std::optional<ui::fx::Slot> = {}) noexcept override
    {
        if (! hasLoaded(scene, track))
            return {};
        return {pixels_.data(), ui::plugin::kScreenWidth, ui::plugin::kScreenHeight, ui::plugin::kScreenWidth, serial_};
    }
    bool screenTouch(int, int, int phase, int x, int y, std::optional<ui::fx::Slot> = {}) noexcept override
    {
        lastTouchPhase = phase;
        lastTouchX = x;
        lastTouchY = y;
        ++serial_;
        ++changes_;
        return true;
    }
    bool screenSelectorTurn(int, int, int delta, std::optional<ui::fx::Slot> = {}) noexcept override
    {
        selectorDelta += delta;
        ++serial_;
        ++changes_;
        return true;
    }
    bool screenSelectorPush(int, int, std::optional<ui::fx::Slot> = {}) noexcept override
    {
        ++selectorPushes;
        return true;
    }
    std::uint32_t changes() const noexcept override { return changes_; }

    int lastTouchPhase = -1, lastTouchX = -1, lastTouchY = -1;
    int selectorDelta = 0, selectorPushes = 0;

private:
    std::vector<ui::plugin::PluginEntry> catalogue_;
    std::vector<std::uint32_t> pixels_;
    std::array<float, 6> knobs_{0.25f, 0.50f, 0.75f, 0.40f, 0.60f, 0.20f};
    std::string preset_{"fm-perc"};
    bool loaded_ = false;
    bool bypassed_ = false;
    std::uint32_t serial_ = 1;
    std::uint32_t changes_ = 0;
};
} // namespace

TEST_CASE("plugin: TrackMode::Plugin model, undoable setPlugin and NativeCodec round-trip", "[plugin][model]")
{
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());

    bus.execute(core::commands::track::setMode(0, 2, core::model::TrackMode::Plugin));
    CHECK(core::commands::project(bus).scenes[0].tracks[2].mode == core::model::TrackMode::Plugin);

    core::model::PluginTrack spec;
    spec.path = "Anvil.clap";
    spec.pluginId = "com.hdslave.anvil";
    spec.name = "Anvil";
    spec.presetKey = "fm-perc";
    spec.stateBase64 = "AAECAwQF";
    spec.bypass = false;
    bus.execute(core::commands::track::setPlugin(0, 2, spec));
    CHECK(core::commands::project(bus).scenes[0].tracks[2].plugin == spec);

    // Undo restores empty plugin track; redo restores spec.
    REQUIRE(bus.undo());
    CHECK(core::commands::project(bus).scenes[0].tracks[2].plugin.path.empty());
    REQUIRE(bus.redo());
    CHECK(core::commands::project(bus).scenes[0].tracks[2].plugin == spec);

    // NativeCodec round-trip preserves TrackMode::Plugin + PluginTrack.
    storetest::ScratchFolder dir("plugin-codec");
    const auto bundle = dir / "PluginTest.sixteen";
    REQUIRE(services::store::NativeCodec::save(core::commands::project(bus), bundle).ok());
    auto loaded = core::model::newDefaultProject();
    services::store::ImportReport report;
    REQUIRE(services::store::NativeCodec::readInto(bundle, *loaded, report).ok());
    CHECK(loaded->scenes[0].tracks[2].mode == core::model::TrackMode::Plugin);
    CHECK(loaded->scenes[0].tracks[2].plugin == spec);
}

TEST_CASE("plugin: DrawCmd::Op::Blit renders 1:1 in SoftRasterizer and tracks FrameDiff serial", "[plugin][render]")
{
    ui::GlyphAtlas atlas;
    auto raster = ui::makeSoftRasterizer(atlas);
    std::vector<std::uint32_t> fb(800 * 480, 0);
    std::vector<std::uint32_t> src(780 * 285, 0xFF112233u);
    src[0] = 0xFFFF0055u;
    src[780 * 284 + 779] = 0xFF00FF88u;

    ui::DrawList dl;
    dl.fill({0, 0, 1280, 768}, 0xFF000000u);
    dl.blit({16, 152, 1248, 456}, src.data(), 780, 285, 780, 1);

    raster->draw(dl, fb.data(), 800, 480, 800);
    // {16, 152, 1248, 456} lpx * 0.625 = {10, 95, 780, 285} physical px.
    CHECK(fb[95 * 800 + 10] == 0x00FF0055u);
    CHECK(fb[95 * 800 + 11] == 0x00112233u);
    CHECK(fb[(95 + 284) * 800 + (10 + 779)] == 0x0000FF88u);
    CHECK(fb[94 * 800 + 10] == 0x00000000u);

    // Damage tracking: same serial => zero damage; bumped serial => damaged rect.
    ui::FrameDamage damage(&atlas);
    CHECK(damage.compute(dl, 800, 480) == ui::FrameDamage::Kind::Full);
    CHECK(damage.compute(dl, 800, 480) == ui::FrameDamage::Kind::None);

    ui::DrawList dl2;
    dl2.fill({0, 0, 1280, 768}, 0xFF000000u);
    dl2.blit({16, 152, 1248, 456}, src.data(), 780, 285, 780, 2);
    CHECK(damage.compute(dl2, 800, 480) == ui::FrameDamage::Kind::Partial);
}

TEST_CASE("plugin: a pending ratchet and its locks follow the playing instrument across a track swap",
          "[plugin][track-swap]")
{
    seqtest::Rig rig;
    auto& original = installPlugin(*rig.e, 0);
    auto& other = installPlugin(*rig.e, 1);
    REQUIRE(rig.param("track.mode", 4, 0));
    REQUIRE(rig.param("track.mode", 4, 1));
    REQUIRE(rig.param("seq.retrigCount", 2, 0));
    REQUIRE(rig.param("seq.retrigSpeed", 5, 0));
    auto before = seqtest::pattern();
    seqtest::put(*before, 0, 0);
    seqtest::lock(*before, 0, 0, 4, 3.0f, core::model::LockChange::Hold);
    REQUIRE(rig.install(*before));
    REQUIRE(rig.play());
    rig.block();
    REQUIRE(original.noteOns == 1);
    REQUIRE(rig.e->perf().retrig.remaining(0) == 1);

    auto after = core::model::newDefaultProject();
    after->scenes[0].patterns[0].length = before->length;
    seqtest::put(after->scenes[0].patterns[0], 0, 1);
    seqtest::lock(after->scenes[0].patterns[0], 0, 1, 4, -5.0f);
    services::render::QueuedBridge queued(rig.e->bridge());
    queued.stageTrackSwap({0, 0, 1, true});
    REQUIRE(ui::input::steps::postPattern(queued, *after, 0, 0));
    postTrackParam(queued, "seq.retrigCount", 0, 1);
    postTrackParam(queued, "seq.retrigCount", 1, 2);
    postTrackParam(queued, "seq.retrigSpeed", 1, 5);
    queued.finishTrackSwaps();

    rig.until(std::lround(seqtest::stepFrames()));
    REQUIRE(rig.e->installed({engine::SwapSlot::TrackPlugin, 0, 1}) == &original);
    CHECK(original.noteOns == 2);
    CHECK(other.noteOns == 0);
    const auto events = rig.e->triggerRouter().blockEvents();
    REQUIRE(events.size() == 1);
    const auto& repeat = events.front();
    REQUIRE(repeat.source == seqtest::TriggerSource::Retrigger);
    CHECK(repeat.track == 1);
    REQUIRE(repeat.locks);
    float pitchLock = 0.0f;
    REQUIRE(repeat.locks->get(4, pitchLock));
    CHECK(pitchLock == 3.0f);
    REQUIRE(repeat.locks->change);
    CHECK(repeat.locks->change[4] == static_cast<std::uint8_t>(core::model::LockChange::Hold));
}

TEST_CASE("plugin: a MIDI release after the swap sends the pitch from note-on", "[plugin][track-swap]")
{
    auto e = miditest::make();
    auto& plugin = installPlugin(*e, 0);
    miditest::Driver driver(*e, engine::kBlockFrames);
    driver.param("track.mode", 0, 4);
    driver.param("playback.timeStretch", 0, 2);
    REQUIRE(e->midi().hub().pushIn(0, miditest::msg(0x90, 72, 100, miditest::hostNs(driver.frame()))));
    driver.blocks(4);
    REQUIRE(plugin.lastNote == 72);

    services::render::QueuedBridge queued(e->bridge());
    queued.stageTrackSwap({0, 0, 1, true});
    postTrackParam(queued, "track.mode", 0, 0);
    postTrackParam(queued, "track.mode", 1, 4);
    postTrackParam(queued, "playback.timeStretch", 0, 0);
    postTrackParam(queued, "playback.timeStretch", 1, 2);
    queued.finishTrackSwaps();
    driver.blocks(2);
    REQUIRE(e->installed({engine::SwapSlot::TrackPlugin, 0, 1}) == &plugin);
    REQUIRE(plugin.sounding);

    driver.param("playback.timeStretch", 1, 0);
    REQUIRE(e->midi().hub().pushIn(0, miditest::msg(0x80, 72, 0, miditest::hostNs(driver.frame()))));
    driver.blocks(4);
    CHECK(plugin.lastOffNote == plugin.lastNote);
    CHECK_FALSE(plugin.sounding);
}

TEST_CASE("plugin: TRANSPOSE moves the instrument's notes and each note ends at its own pitch", "[plugin][engine]")
{
    auto e = miditest::make();
    auto& plugin = installPlugin(*e, 0);
    miditest::Driver driver(*e, engine::kBlockFrames);
    driver.param("track.mode", 0, 4);
    const auto send = [&](std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
    {
        REQUIRE(e->midi().hub().pushIn(0, miditest::msg(status, data1, data2, miditest::hostNs(driver.frame()))));
        driver.blocks(4);
    };

    send(0x90, 48, 100);
    CHECK(plugin.lastNote == 60); // MIDI IN note 48 is the track's own pitch (MID-021); TRANSPOSE 0 adds nothing
    send(0x80, 48, 0);
    CHECK(plugin.lastOffNote == 60);

    driver.param("seq.transpose", 0, 7);
    driver.blocks(1);
    send(0x90, 48, 100);
    CHECK(plugin.lastNote == 67);

    // TRANSPOSE moves while the key is held: the release still ends the note that sounded.
    driver.param("seq.transpose", 0, -5);
    driver.blocks(1);
    const int offs = plugin.noteOffs;
    send(0x80, 48, 0);
    CHECK(plugin.lastOffNote == 67);
    CHECK(plugin.noteOffs == offs + 1);
    CHECK_FALSE(plugin.sounding);

    send(0x90, 48, 100);
    CHECK(plugin.lastNote == 55);
    send(0x80, 48, 0);
    CHECK(plugin.lastOffNote == 55);
}

TEST_CASE("plugin: a key struck again after TRANSPOSE moved ends its earlier pitch first", "[plugin][engine]")
{
    using namespace engine::trigger;
    auto rig = trigtest::makeRig();
    MockPluginNode mock(nullptr);
    rig->src[0].setMode(TrackMode::Plugin);
    rig->src[0].setPlugin(&mock);

    rig->src[0].setNoteTranspose(2);
    REQUIRE(rig->router.pushLive(engine::TriggerNow{0, 100, 60, 0}));
    rig->block();
    REQUIRE(mock.lastNote == 62);

    rig->src[0].setNoteTranspose(12);
    REQUIRE(rig->router.pushLive(engine::TriggerNow{0, 100, 60, 0})); // a second note on for the held key
    rig->block();
    CHECK(mock.lastOffNote == 62);
    CHECK(mock.lastNote == 72);

    const int offs = mock.noteOffs;
    REQUIRE(rig->router.pushLive(engine::TriggerNow{0, 0, 60, 0}));
    rig->block();
    CHECK(mock.lastOffNote == 72);
    CHECK(mock.noteOffs == offs + 1);
    rig->src[0].setPlugin(nullptr);
}

TEST_CASE("plugin: a held transposed note keeps its pitch through a track swap", "[plugin][track-swap]")
{
    auto e = miditest::make();
    auto& plugin = installPlugin(*e, 0);
    miditest::Driver driver(*e, engine::kBlockFrames);
    driver.param("track.mode", 0, 4);
    driver.param("seq.transpose", 0, 7);
    driver.blocks(1);
    REQUIRE(e->midi().hub().pushIn(0, miditest::msg(0x90, 48, 100, miditest::hostNs(driver.frame()))));
    driver.blocks(4);
    REQUIRE(plugin.lastNote == 67);

    services::render::QueuedBridge queued(e->bridge());
    queued.stageTrackSwap({0, 0, 1, true});
    postTrackParam(queued, "track.mode", 0, 0);
    postTrackParam(queued, "track.mode", 1, 4);
    queued.finishTrackSwaps();
    driver.blocks(2);
    REQUIRE(e->installed({engine::SwapSlot::TrackPlugin, 0, 1}) == &plugin);
    REQUIRE(plugin.sounding);

    driver.param("seq.transpose", 1, 0); // the instrument's new track moves to another TRANSPOSE while it is held
    REQUIRE(e->midi().hub().pushIn(0, miditest::msg(0x80, 48, 0, miditest::hostNs(driver.frame()))));
    driver.blocks(4);
    CHECK(plugin.lastOffNote == 67);
    CHECK_FALSE(plugin.sounding);
}

TEST_CASE("plugin: MIDI All Notes Off still reaches the original channel's held note after a swap",
          "[plugin][track-swap]")
{
    auto e = miditest::make();
    auto& channelOne = installPlugin(*e, 0);
    auto& channelTwo = installPlugin(*e, 1);
    miditest::Driver driver(*e, engine::kBlockFrames);
    driver.param("track.mode", 0, 4);
    driver.param("track.mode", 1, 4);
    const auto send = [&](std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
    {
        REQUIRE(e->midi().hub().pushIn(0, miditest::msg(status, data1, data2, miditest::hostNs(driver.frame()))));
        driver.blocks(4);
    };
    send(0x90, 48, 100);
    send(0x91, 60, 100);
    REQUIRE(channelOne.sounding);
    REQUIRE(channelTwo.sounding);

    services::render::QueuedBridge queued(e->bridge());
    queued.stageTrackSwap({0, 0, 1, true});
    queued.finishTrackSwaps();
    driver.blocks(1);
    REQUIRE(e->installed({engine::SwapSlot::TrackPlugin, 0, 1}) == &channelOne);
    send(0xB0, 123, 0);
    CHECK_FALSE(channelOne.sounding);
    CHECK(channelTwo.sounding);
    send(0xB1, 123, 0);
    CHECK_FALSE(channelTwo.sounding);
}

TEST_CASE("plugin: a running volume LFO stays sample-exact through a track swap", "[plugin][track-swap]")
{
    auto control = enginetest::make(false);
    auto moved = enginetest::make(false);
    const auto lfo = [](engine::EngineBridge& bridge, std::uint16_t track)
    {
        postTrackParam(bridge, "lfo.shape", track, static_cast<int>(engine::modulation::LfoShape::Sine));
        postTrackParam(bridge, "lfo.speed", track, 100);
        postTrackParam(bridge, "lfo.depth", track, 100);
        postTrackParam(bridge, "lfo.triggerMode", track, 1);
        postTrackParam(bridge, "lfo.destination", track, static_cast<float>(miditest::id("track.volume").value));
    };
    const auto configure = [&](engine::Engine& e)
    {
        installPlugin(e, 0);
        postTrackParam(e.bridge(), "track.mode", 0, 4);
        lfo(e.bridge(), 0);
        REQUIRE(e.bridge().post(engine::TransportCmd{engine::TransportCmd::Play, 0.0}));
        enginetest::render(e, 4096);
        REQUIRE(e.bridge().post(engine::TriggerNow{0, 127, 60, 0}));
        return enginetest::render(e, 1024);
    };
    const auto before = configure(*control);
    REQUIRE(configure(*moved).l == before.l);

    services::render::QueuedBridge queued(moved->bridge());
    queued.stageTrackSwap({0, 0, 1, true});
    postTrackParam(queued, "track.mode", 0, 0);
    postTrackParam(queued, "track.mode", 1, 4);
    lfo(queued, 1);
    postTrackParam(queued, "lfo.destination", 0, -1);
    queued.finishTrackSwaps();

    const auto reference = enginetest::render(*control, 256);
    const auto candidate = enginetest::render(*moved, 256);
    CHECK(candidate.l == reference.l);
    CHECK(candidate.r == reference.r);
    CHECK(moved->lfo().lfo(1).elapsed() == control->lfo().lfo(0).elapsed());
}

TEST_CASE("plugin: touch-strip repeats and pad pressure follow a held note across a track swap", "[plugin][track-swap]")
{
    auto e = enginetest::make(false);
    auto& original = installPlugin(*e, 0);
    auto& other = installPlugin(*e, 1);
    postTrackParam(e->bridge(), "track.mode", 0, 4);
    postTrackParam(e->bridge(), "track.mode", 1, 4);
    postTrackParam(e->bridge(), "ribbon.mode", 0, static_cast<int>(core::ribbon::Mode::Repeat));
    REQUIRE(e->bridge().post(engine::TriggerNow{0, 127, 1, core::ribbon::kStripPad}));
    REQUIRE(e->bridge().post(engine::TriggerNow{0, 100, 60, 0}));
    enginetest::render(*e, 2 * engine::kBlockFrames);
    REQUIRE(original.noteOns == 1);
    REQUIRE(e->ribbon().repeating());

    services::render::QueuedBridge queued(e->bridge());
    queued.stageTrackSwap({0, 0, 1, true});
    queued.finishTrackSwaps();
    REQUIRE(queued.post(engine::TriggerNow{0, 64, 0, core::ribbon::kPressurePad}));
    enginetest::render(*e, 4096);
    CHECK(original.noteOns == 2);
    CHECK(other.noteOns == 0);
    CHECK(original.lastVel == 64.0f * (1.0f / 127.0f));

    REQUIRE(queued.post(engine::TriggerNow{0, 0, 60, 0}));
    enginetest::render(*e, 4096);
    CHECK(original.noteOffs == 1);
    CHECK(original.noteOns == 2);
    CHECK_FALSE(original.sounding);
    CHECK_FALSE(e->ribbon().repeating());
}

TEST_CASE("plugin: Engine SwapSlot::TrackPlugin renders audio on triggers and retires cleanly", "[plugin][engine]")
{
    const auto& params = core::builtinParams();
    auto resolve = [&](std::string_view id) { return params.idOf(id).value_or(core::ParamId{}); };
    engine::EngineSetup setup{};
    setup.resolveParam = resolve;
    setup.testTone = false;

    auto eng = std::make_unique<engine::Engine>(setup);
    eng->prepare(44100.0, 256);

    std::atomic<int> destroyed{0};
    auto* mock = new MockPluginNode(&destroyed);
    mock->retain(); // engine's reference

    REQUIRE(
        eng->bridge().post(engine::ParamChange{*params.idOf("track.mode"), 0, 4.0f, 0})); // scene 0 track 0 = Plugin
    REQUIRE(eng->bridge().post(engine::ObjectSwap{{engine::SwapSlot::TrackPlugin, 0, 0}, mock}));
    REQUIRE(eng->bridge().post(engine::TriggerNow{0, 110, 64, 0}));

    std::array<float, 256> outL{}, outR{};
    float* outs[2] = {outL.data(), outR.data()};
    eng->process(engine::AudioIO{outs, 1, nullptr, 0, 256, 0});

    CHECK(mock->noteOns == 1);
    CHECK(mock->lastNote == 64);
    float peakL = 0.0f;
    for (float s : outL)
        peakL = std::max(peakL, std::fabs(s));
    CHECK(peakL > 0.05f);

    REQUIRE(eng->bridge().post(engine::TransportCmd{engine::TransportCmd::Play, 0.0}));
    enginetest::render(*eng, 256);
    REQUIRE(eng->bridge().post(engine::TransportCmd{engine::TransportCmd::Stop, 1.0}));
    CHECK(enginetest::peak(enginetest::render(*eng, 1024).l, 512) > 0.05f);
    CHECK(mock->resets == 0);
    REQUIRE(eng->bridge().post(engine::TransportCmd{engine::TransportCmd::Stop, 2.0}));
    CHECK(enginetest::peak(enginetest::render(*eng, 1024).l, 512) == 0.0f);
    CHECK(mock->resets == 1);

    // Replace with nullptr and drain RetireQueue: engine drops its reference.
    REQUIRE(eng->bridge().post(engine::ObjectSwap{{engine::SwapSlot::TrackPlugin, 0, 0}, nullptr}));
    eng->process(engine::AudioIO{outs, 1, nullptr, 0, 256, 0});
    eng->bridge().retirePending();
    CHECK(destroyed.load() == 0); // test still owns its initial ref
    mock->release();
    CHECK(destroyed.load() == 1);
}

TEST_CASE("plugin: two Stop presses preserve then silence a loaded CLAP instrument", "[plugin][engine][stop-audio]")
{
    auto engine = enginetest::make(false);
    auto& bridge = engine->bridge();
    const auto path = juce::File(CLAP_FIXTURE_DIR).getChildFile("clap_effect_fixture.clap");
    std::string error;
    std::unique_ptr<services::plugin::ClapAudioNode> instrument(
        services::plugin::ClapAudioNode::create(path.getFullPathName().toStdString(), "test.instrument", 44100, error));
    INFO(error);
    REQUIRE(instrument);
    REQUIRE(bridge.post(engine::ObjectSwap{{engine::SwapSlot::TrackPlugin, 0, 0}, instrument.get()}));
    instrument.release(); // the engine now owns this reference
    REQUIRE(bridge.post(engine::ParamChange{*core::builtinParams().idOf("track.mode"), 0, 4.0f, 0}));

    const engine::TriggerNow hit{0, 110, 64, 0};
    auto level = [&] { return enginetest::peak(enginetest::render(*engine, 2048).l, 1024); };
    REQUIRE(bridge.post(engine::TransportCmd{engine::TransportCmd::Play, 0.0}));
    REQUIRE(bridge.post(hit));
    REQUIRE(level() > 0.05f);
    REQUIRE(bridge.post(engine::TransportCmd{engine::TransportCmd::Stop, 1.0}));
    CHECK(level() > 0.05f);
    REQUIRE(bridge.post(engine::TransportCmd{engine::TransportCmd::Stop, 1.0}));
    CHECK(level() == 0.0f);
    REQUIRE(bridge.post(hit));
    CHECK(level() > 0.05f);
}

TEST_CASE("CLAP effects process live track audio in both inserts and the master, including undo and bypass",
          "[plugin][fx][engine]")
{
    auto setup = enginetest::fullSetup();
    setup.testTone = false;
    auto engine = std::make_unique<engine::Engine>(setup);
    engine->prepare(44100, 256);
    auto& bridge = engine->bridge();
    services::plugin::ClapHostService host(bridge, CLAP_FIXTURE_DIR, 44100);
    core::CommandBus bus;
    core::commands::replaceProject(bus, core::model::newDefaultProject());
    core::commands::setBridge(bus,
                              [&](const auto& changes, const auto& p)
                              {
                                  host.syncChanges(changes, p);
                                  ui::fx::forwardFxChanges(changes, p, bridge);
                              });
    const auto& params = core::builtinParams();
    REQUIRE(bridge.post(engine::ObjectSwap{{engine::SwapSlot::TrackPlugin, 0, 0}, new MockPluginNode(nullptr)}));
    REQUIRE(bridge.post(engine::ParamChange{*params.idOf("track.mode"), 0, 4.0f, 0}));
    REQUIRE(bridge.post(engine::TriggerNow{0, 110, 64, 0}));
    // Compare steady-state peaks after each slot's native activation/bypass ramps.
    auto level = [&] { return enginetest::peak(enginetest::render(*engine, 4096).l, 3072); };
    const float dry = level();
    REQUIRE(dry > 0.05f);
    core::model::PluginTrack spec{.path = "clap_effect_fixture.clap", .pluginId = "test.stereo", .name = "Stereo gain"};
    app::PluginUiAdapter adapter(bus, host);
    adapter.rescan();
    const auto catalogue = adapter.catalogue();
    const auto stereo =
        std::find_if(catalogue.begin(), catalogue.end(), [](const auto& e) { return e.id == "test.stereo"; });
    REQUIRE(stereo != catalogue.end());
    const auto master = ui::fx::pathOf(ui::fx::Slot::Master, 1, 1);
    float expected = dry;
    for (auto s : {ui::fx::Slot::Fx1, ui::fx::Slot::Fx2, ui::fx::Slot::Master})
    {
        REQUIRE(adapter.selectEffectPlugin(s, 1, 1, *stereo));
        CHECK(adapter.effectPlugin(s, 1, 1).loaded);
        expected *= 0.25f;
        CHECK(std::fabs(level() - expected) < 0.0001f);
    }
    auto missing = *stereo;
    missing.path = "removed.clap";
    CHECK_FALSE(adapter.selectEffectPlugin(ui::fx::Slot::Master, 1, 1, missing));
    CHECK(adapter.effectPlugin(ui::fx::Slot::Master, 1, 1).path == stereo->path);
    CHECK(std::fabs(level() - expected) < 0.0001f);
    bus.execute(core::makeSetFxBypass(master, true));
    CHECK(std::fabs(level() - dry / 16) < 0.0001f);
    REQUIRE(bus.undo());
    CHECK(std::fabs(level() - expected) < 0.0001f);

    REQUIRE(bridge.post(engine::ParamChange{*params.idOf("track.sendAmount"), 0, 127, 0}));
    const auto send = ui::fx::pathOf(ui::fx::Slot::Send, 1, 1);
    bus.execute(std::make_unique<core::SelectFxType>(send, core::model::EffectType::Plugin, spec));
    CHECK(level() > expected + 0.0001f);
    REQUIRE(bus.undo());
    CHECK(std::fabs(level() - expected) < 0.0001f);

    spec.path = "removed.clap";
    bus.execute(std::make_unique<core::SelectFxType>(send, core::model::EffectType::Plugin, spec));
    for (float mix : {1.0f, 0.5f, 0.0f})
    {
        REQUIRE(bridge.post(engine::ParamChange{*params.idOf("send.plugin.mix"), 0, mix, 0}));
        CHECK(std::fabs(level() - expected) < 0.0001f);
    }
}

TEST_CASE("plugin: overlapping scale pads release their own notes", "[plugin][engine]")
{
    using namespace engine::trigger;
    auto rig = trigtest::makeRig();
    MockPluginNode mock(nullptr);
    rig->src[0].setMode(TrackMode::Plugin);
    rig->src[0].setPlugin(&mock);
    rig->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Scale));

    for (const auto& order : {std::array{0, 1}, std::array{1, 0}})
    {
        CAPTURE(order[0]);
        const int noteOffs = mock.noteOffs;
        REQUIRE(rig->router.pushLive(engine::TriggerNow{0, 100, 60, 0}));
        REQUIRE(rig->router.pushLive(engine::TriggerNow{1, 100, 60, 1}));
        rig->block();
        CHECK(mock.lastNote == 61);

        // Releasing after a scale edit must still stop the note that sounded on the press.
        rig->param(0, TrackField::KeyShift, 5.0f);
        for (int pad : order)
        {
            REQUIRE(rig->router.pushLive(
                engine::TriggerNow{static_cast<std::uint8_t>(pad), 0, 60, static_cast<std::int8_t>(pad)}));
            rig->block();
            CHECK(mock.lastOffNote == 60 + pad);
        }
        CHECK(mock.noteOffs == noteOffs + 2);
        rig->param(0, TrackField::KeyShift, 0.0f);
    }
    rig->src[0].setPlugin(nullptr);
}

TEST_CASE("plugin: TRACK TYPE -> PLUGIN (mode-only) -> SELECT PLUGIN -> track.plugin custom UI + 6 knobs",
          "[plugin][ui]")
{
    MockCustomScreenAccess access;
    ui::plugin::ScopedPluginEnvironment scoped({&access});

    ui::ShellConfig cfg{core::builtinParams(),
                        [](ui::TextStyleId, std::string_view t) { return 8.0f * static_cast<float>(t.size()); }};
    ui::Shell shell(std::move(cfg));
    ui::DrawList dl;
    dl.reserve(4096, 32768);
    double now = 1000.0;
    auto frame = [&]()
    {
        now += 500.0;
        dl.clear();
        shell.frame(dl, now);
    };
    auto send = [&](io::ControlEvent::Kind k, io::Control c, int v = 0)
    { shell.dispatch({k, c, static_cast<std::int16_t>(v), io::kSourceVirtual, now}); };
    auto touch = [&](float cx, float cy, std::int16_t phase = ui::touch::kDown)
    {
        io::ControlEvent e{io::ControlEvent::Kind::Touch, io::Control::Screen, phase, io::kSourceVirtual, now};
        e.x = static_cast<std::int16_t>(cx / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(cy / ui::touch::kToCanvas + 0.5f);
        shell.dispatch(e);
    };

    REQUIRE(shell.open("track.menu"));
    frame();

    // Open TRACK TYPE popup from context bar.
    touch(932, 88);
    frame();
    REQUIRE(shell.popupDepth() == 1);

    // Step 4 times to PLUGIN (SAMPLE -> THRU -> MIDI -> AS-1 -> PLUGIN) and confirm.
    send(io::ControlEvent::Kind::Turn, io::Control::Selector, 4);
    frame();
    send(io::ControlEvent::Kind::Press, io::Control::SelectorPush);
    frame();

    // A3 check: mode switches to Plugin (4) ONLY — no auto-pushed second popup!
    CHECK(shell.popupDepth() == 0);
    const auto* modeSpec = core::builtinParams().find("track.mode");
    REQUIRE(modeSpec != nullptr);
    CHECK(shell.values().get(*modeSpec, shell.state().trackScope()) == 4.0f);
    CHECK(shell.contextBar().model().trackType == "PLUGIN");
    // Open focus on Plugin TRACK MENU is SELECT PLUGIN (slot 2).
    CHECK(shell.stripTab() == "SELECT PLUGIN");

    // Push selector on focused SELECT PLUGIN -> opens plugin.select popup.
    send(io::ControlEvent::Kind::Press, io::Control::SelectorPush);
    frame();
    REQUIRE(shell.popupDepth() == 1);

    // Move down from NONE to Anvil and choose.
    send(io::ControlEvent::Kind::Turn, io::Control::Selector, 1);
    frame();
    send(io::ControlEvent::Kind::Press, io::Control::SelectorPush);
    frame();
    CHECK(shell.popupDepth() == 0);
    CHECK(access.hasLoaded(1, 1));

    // Open track.plugin (custom body screen inside standard chrome).
    REQUIRE(shell.open("track.plugin"));
    frame();
    CHECK(shell.router().current().id == "track.plugin");
    CHECK(shell.stripTab() == "VCO");
    REQUIRE(shell.column(0).spec != nullptr);
    CHECK(shell.column(0).spec->label == "VCO1 FREQ");
    CHECK(shell.column(5).spec != nullptr);
    CHECK(shell.column(5).spec->label == "FM AMT");

    bool sawBlit = false;
    for (const auto& cmd : dl.commands())
        if (cmd.op == ui::DrawCmd::Op::Blit && cmd.srcW == 780 && cmd.srcH == 285)
            sawBlit = true;
    CHECK(sawBlit);

    auto visible = [&](std::string_view text)
    {
        return std::any_of(dl.commands().begin(), dl.commands().end(), [&](const ui::DrawCmd& cmd)
                           { return cmd.op == ui::DrawCmd::Op::Text && dl.textOf(cmd) == text; });
    };
    REQUIRE(visible("PRESETS"));
    touch(1190, 88);
    frame();
    REQUIRE(shell.popupDepth() == 1);
    CHECK(visible("FM Perc"));
    CHECK(visible("Kick"));
    send(io::ControlEvent::Kind::Turn, io::Control::Selector, 1);
    CHECK(access.currentPreset(1, 1) == "fm-perc");
    send(io::ControlEvent::Kind::Press, io::Control::SelectorPush);
    frame();
    CHECK(shell.popupDepth() == 0);
    CHECK(access.currentPreset(1, 1) == "kick");
    touch(1190, 88);
    frame();
    send(io::ControlEvent::Kind::Turn, io::Control::Selector, -1);
    send(io::ControlEvent::Kind::Press, io::Control::Back);
    frame();
    CHECK(shell.popupDepth() == 0);
    CHECK(access.currentPreset(1, 1) == "kick");
    CHECK(access.selectorPushes == 0);
    CHECK(access.selectorDelta == 0);
    CHECK(access.lastTouchPhase == -1);

    auto enterName = [&]
    {
        touch(1190, 88);
        frame();
        REQUIRE(visible("SAVE PRESET..."));
        send(io::ControlEvent::Kind::Turn, io::Control::Selector, -8);
        send(io::ControlEvent::Kind::Press, io::Control::SelectorPush);
        frame();
        REQUIRE(visible("SAVE PLUGIN PRESET"));
        shell.typeText("Night Drive");
        shell.textKey(ui::TextKey::Return);
        frame();
    };
    enterName();
    REQUIRE(access.savedName == "Night Drive");
    REQUIRE(access.writes == 1);
    enterName();
    REQUIRE(visible("REPLACE PRESET"));
    send(io::ControlEvent::Kind::Press, io::Control::Back);
    frame();
    REQUIRE(access.writes == 1);
    enterName();
    send(io::ControlEvent::Kind::Turn, io::Control::Selector, 1);
    send(io::ControlEvent::Kind::Press, io::Control::SelectorPush);
    frame();
    REQUIRE(access.writes == 2);
    touch(1190, 88);
    frame();
    REQUIRE(visible("USER"));
    REQUIRE(visible("Night Drive"));
    send(io::ControlEvent::Kind::Press, io::Control::Back);
    frame();

    // Turn hardware knob 1 -> updates plugin param 0 via StripOwner::stripTurnUnbound.
    const float before = shell.column(0).value;
    send(io::ControlEvent::Kind::Turn, io::knob(1), 2);
    frame();
    CHECK(shell.column(0).value > before);

    // Touch inside body canvas {16, 152, 1248, 456} maps cleanly to plugin 0..779 x 0..284.
    touch(16 + 160, 152 + 80, ui::touch::kDown); // physical (100, 50)
    CHECK(access.lastTouchPhase == 0);
    CHECK(access.lastTouchX == 100);
    CHECK(access.lastTouchY == 50);
}
