// Plugin tracks: TrackMode::Plugin model + undo + codec round-trip, DrawCmd::Op::Blit raster & damage,
// Engine SwapSlot::TrackPlugin + TrackSource triggers/audio/retirement, and TRACK TYPE -> PLUGIN -> SELECT PLUGIN ->
// track.plugin custom body screen + 6-knob StripOwner flow.
#include "core/audio/PluginNode.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "core/model/enums/EnumStrings.h"
#include "core/params/ParamRegistry.h"
#include "engine/Engine.h"
#include "services/store/NativeCodec.h"
#include "tests/unit/store/StoreTestSupport.h"
#include "ui/render/Damage.h"
#include "ui/render/DrawList.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"
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

    int lastNote = -1;
    float lastVel = 0.0f;
    int noteOns = 0, noteOffs = 0, allOffs = 0;
    bool sounding = false;

private:
    std::atomic<int>* destroyed_;
};

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
        return hasLoaded(scene, track) ? std::string_view("Anvil") : std::string_view {};
    }
    std::string_view loadedVendor(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("Test") : std::string_view {};
    }
    std::string_view loadedId(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("com.test.anvil") : std::string_view {};
    }
    std::string_view loadedPath(int scene, int track) const noexcept override
    {
        return hasLoaded(scene, track) ? std::string_view("Anvil.clap") : std::string_view {};
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
    std::vector<ui::plugin::PluginPresetEntry> presets(int, int) const override
    {
        return loaded_ ? std::vector<ui::plugin::PluginPresetEntry>{{"fm-perc", "FM Perc"}, {"kick", "Kick"}}
                       : std::vector<ui::plugin::PluginPresetEntry>{};
    }
    std::string_view currentPreset(int, int) const noexcept override { return loaded_ ? preset_ : std::string_view {}; }
    bool loadPreset(int, int, std::string_view key) override
    {
        if (! loaded_)
            return false;
        preset_ = std::string(key);
        ++changes_;
        return true;
    }
    bool readKnobs(int scene, int track, ui::plugin::PluginKnobBank& out) const noexcept override
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
    bool turnKnob(int scene, int track, int col0, int detents, bool fine) noexcept override
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
    bool hasCustomScreen(int scene, int track) const noexcept override { return hasLoaded(scene, track); }
    ui::plugin::PluginScreenFrame screenRender(int scene, int track, std::uint32_t) noexcept override
    {
        if (! hasLoaded(scene, track))
            return {};
        return {pixels_.data(), ui::plugin::kScreenWidth, ui::plugin::kScreenHeight, ui::plugin::kScreenWidth, serial_};
    }
    bool screenTouch(int, int, int phase, int x, int y) noexcept override
    {
        lastTouchPhase = phase;
        lastTouchX = x;
        lastTouchY = y;
        ++serial_;
        ++changes_;
        return true;
    }
    bool screenSelectorTurn(int, int, int delta) noexcept override
    {
        selectorDelta += delta;
        ++serial_;
        ++changes_;
        return true;
    }
    bool screenSelectorPush(int, int) noexcept override
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
    std::array<float, 6> knobs_ {0.25f, 0.50f, 0.75f, 0.40f, 0.60f, 0.20f};
    std::string preset_ {"fm-perc"};
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

TEST_CASE("plugin: Engine SwapSlot::TrackPlugin renders audio on triggers and retires cleanly", "[plugin][engine]")
{
    const auto& params = core::builtinParams();
    auto resolve = [&](std::string_view id) { return params.idOf(id).value_or(core::ParamId {}); };
    engine::EngineSetup setup {};
    setup.resolveParam = resolve;
    setup.testTone = false;

    auto eng = std::make_unique<engine::Engine>(setup);
    eng->prepare(44100.0, 256);

    std::atomic<int> destroyed {0};
    auto* mock = new MockPluginNode(&destroyed);
    mock->retain(); // engine's reference

    REQUIRE(eng->bridge().post(engine::ParamChange {*params.idOf("track.mode"), 0, 4.0f, 0})); // scene 0 track 0 = Plugin
    REQUIRE(eng->bridge().post(engine::ObjectSwap {{engine::SwapSlot::TrackPlugin, 0, 0}, mock}));
    REQUIRE(eng->bridge().post(engine::TriggerNow {0, 110, 64, 0}));

    std::array<float, 256> outL {}, outR {};
    float* outs[2] = {outL.data(), outR.data()};
    eng->process(engine::AudioIO {outs, 1, nullptr, 0, 256, 0});

    CHECK(mock->noteOns == 1);
    CHECK(mock->lastNote == 64);
    float peakL = 0.0f;
    for (float s : outL)
        peakL = std::max(peakL, std::fabs(s));
    CHECK(peakL > 0.05f);

    // Replace with nullptr and drain RetireQueue: engine drops its reference.
    REQUIRE(eng->bridge().post(engine::ObjectSwap {{engine::SwapSlot::TrackPlugin, 0, 0}, nullptr}));
    eng->process(engine::AudioIO {outs, 1, nullptr, 0, 256, 0});
    eng->bridge().retirePending();
    CHECK(destroyed.load() == 0); // test still owns its initial ref
    mock->release();
    CHECK(destroyed.load() == 1);
}

TEST_CASE("plugin: TRACK TYPE -> PLUGIN (mode-only) -> SELECT PLUGIN -> track.plugin custom UI + 6 knobs", "[plugin][ui]")
{
    MockCustomScreenAccess access;
    ui::plugin::ScopedPluginEnvironment scoped({&access});

    ui::ShellConfig cfg {core::builtinParams(),
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
        io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen, phase, io::kSourceVirtual, now};
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
