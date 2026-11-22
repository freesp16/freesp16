// Every APP_REGISTER_* macro registers from a translation unit nobody references (static registrars,
// whole-archive linking), and the registries find what was registered. This file registers one test descriptor per
// extension point, exactly as a feature file would.
#include "core/params/ParamRegistry.h"
#include "engine/fx/EffectBase.h"
#include "engine/voice/TimeStretcher.h"
#include "io/controllers/ControlSurface.h"
#include "platform/Platform.h"
#include "services/store/ProjectCodec.h"
#include "ui/descriptors/EffectSpec.h"
#include "ui/descriptors/ModuleSpec.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/descriptors/ScreenSpec.h"
#include "ui/render/RendererRegistry.h"
#include "ui/widgets/Widget.h"

#include "tests/fakes/FakeControlSource.h"
#include "tests/fakes/FakeEffect.h"
#include "tests/fakes/FakeStretcher.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace
{
// parameters (§5.2)
void addContractParams(core::ParamRegistry& r)
{
    r.add({.id = "contract.amount", .label = "AMOUNT", .shortLabel = "AMOUNT", .group = "CONTRACT", .minValue = 0,
           .maxValue = 1, .lockable = true, .scope = core::ParamScope::Scene});
    r.add({.id = "contract.pref", .label = "PREF", .shortLabel = "PREF", .group = "CONTRACT", .type = core::ParamType::Bool,
           .scope = core::ParamScope::Global, .storage = {.key = "prefs.contract"}});
}

// screens + widgets (§5.1)
class Box final : public ui::Widget
{
public:
    ui::RectF bounds() const noexcept override { return {0, 0, 10, 10}; }
    void build(ui::DrawList& dl, const ui::UiContext&) const override { dl.fill(bounds(), 0xFF000000u); }
};
std::unique_ptr<ui::Widget> makeBox(const ui::ScreenSpec&, const ui::WidgetArgs&) { return std::make_unique<Box>(); }
constexpr ui::RailItemSpec contractRail[] = {{ui::RailItemSpec::Kind::Button, "CHILD", "contract.child"},
                                             {ui::RailItemSpec::Kind::Toggle, "PREF", {}, "contract.pref"}};

// effects (§5.3)
constexpr std::string_view contractFxOptions[] = {"contract.pref"};

// codecs (§5.6)
class NullCodec final : public services::store::ProjectCodec
{
public:
    services::store::CodecInfo info() const override { return {"contract-null", "NULL", {}, false}; }
    bool canRead(const juce::File&) const override { return false; }
    core::Result<std::unique_ptr<core::model::Project>> read(const juce::File&,
                                                             services::store::ImportReport&) const override
    {
        return core::Result<std::unique_ptr<core::model::Project>>::failure("not supported");
    }
    core::Result<void> write(const core::model::Project&, const juce::File&) const override
    {
        return core::Result<void>::failure("not supported");
    }
};
void migrateNothing(juce::var&) {}

// control sources (§5.7)
std::unique_ptr<io::ControlSource> makeVirtual(engine::TriggerSink* t) { return std::make_unique<fakes::FakeControlSource>(t, "contract-virtual"); }

// renderers (§5.5)
std::unique_ptr<ui::Renderer> makeNothing(const ui::RendererEnv&, std::string& error)
{
    error = "contract renderer never draws";
    return nullptr;
}

std::unique_ptr<engine::voice::TimeStretcher> makeFakeStretcher() { return std::make_unique<fakes::FakeStretcher>(); }

void contractList(std::string_view, std::vector<ui::ListItem>& out) { out.push_back({"ONE", "G", {}, 1}); }
} // namespace

APP_REGISTER_PARAMS("zz_contract", 9000, &addContractParams)
APP_REGISTER_WIDGET("ContractBox", &makeBox)
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "contract.parent", .title = "CONTRACT", .parent = "", .body = ui::BodyTemplate::Full,
                                     .contextBar = ui::ContextBarKind::Menu, .rail = contractRail,
                                     .bodyComponent = "ContractBox", .strip = {"CONTRACT", {"contract.amount"}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "contract.child", .title = "CHILD", .parent = "contract.parent",
                                     .body = ui::BodyTemplate::RailContent, .contextBar = ui::ContextBarKind::None}))
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor {"contract-gain", "GAIN", engine::fx::SlotKind::Insert | engine::fx::SlotKind::Send,
                                                   {"contract.amount", "", "", "", "", ""}, contractFxOptions, "meters",
                                                   {}, &engine::fx::make<fakes::FakeEffect>}))
APP_REGISTER_EFFECT_UI((ui::EffectUiSpec {"contract-gain", "gain", {}}))
APP_REGISTER_STRETCHER("contract-resample", &makeFakeStretcher)
APP_REGISTER_CODEC(NullCodec)
APP_REGISTER_MIGRATION({99, 100, &migrateNothing, "contract: no-op"}) // beyond kSchemaVersion: never in a chain
APP_REGISTER_CONTROL_SOURCE("contract-virtual", &makeVirtual)
APP_REGISTER_RENDERER("contract-null", &makeNothing, false)
APP_REGISTER_MODULE((ui::ModuleSpec {"contract.module", "CONTRACT", "WaveformThumb", "contract.strip", "contract.child"}))
APP_REGISTER_LIST_SOURCE("contract.list", &contractList)
APP_REGISTER_POPUP((ui::PopupSpec {ui::PopupSpec::Kind::Toast, "contract.toast", "SAVED"}))
APP_REGISTER_SETTINGS((ui::SettingsRowEntry {"utility", "contract", 10, {ui::SettingsRowSpec::Kind::Value, "PREF", "contract.pref"}}))
APP_REGISTER_SETTINGS((ui::SettingsRowEntry {"utility", "contract", 5, {ui::SettingsRowSpec::Kind::Info, "VERSION"}}))
APP_REGISTER_SETTINGS((ui::SettingsSectionSpec {"utility", "contract", "CONTRACT", 99}))

TEST_CASE("APP_REGISTER_* registrars reach their registries", "[contracts][registry]")
{
    const auto& params = core::builtinParams();
    CHECK(params.errors().empty());
    REQUIRE(params.find("contract.amount") != nullptr);
    CHECK(params.find("contract.pref")->storageKey() == "prefs.contract");
    CHECK(params.find("contract.amount")->storageKey() == "contract.amount");
    const auto prefs = params.byScope(core::ParamScope::Global); // the contract's preference among the built-in ones
    CHECK(std::count(prefs.begin(), prefs.end(), *params.idOf("contract.pref")) == 1);
    // catalogues run sorted by (order, area): track (100) before playback (200) before mixer (500) before the contract
    CHECK(params.idOf("track.triggerMode")->value < params.idOf("playback.pitch")->value);
    CHECK(params.idOf("playback.pitch")->value < params.idOf("track.volume")->value);
    CHECK(params.idOf("playback.loop")->value < params.idOf("contract.amount")->value);

    const auto& screens = ui::ScreenRegistry::builtin();
    REQUIRE(screens.find("contract.parent") != nullptr);
    CHECK(screens.find("contract.child")->parent == "contract.parent");
    const auto problems = screens.validate(ui::WidgetRegistry::builtin().dispatcher(), params);
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());

    const auto* fx = engine::fx::EffectRegistry::builtin().find("contract-gain");
    REQUIRE(fx != nullptr);
    CHECK(engine::fx::allows(fx->slots, engine::fx::SlotKind::Send));
    CHECK_FALSE(engine::fx::allows(fx->slots, engine::fx::SlotKind::Master));
    auto instance = fx->create();
    REQUIRE(instance != nullptr);
    CHECK(ui::EffectUiRegistry::builtin().find("contract-gain")->icon == "gain");

    REQUIRE(engine::voice::StretcherRegistry::builtin().find("contract-resample") != nullptr);
    CHECK(engine::voice::StretcherRegistry::builtin().find("contract-resample")()->latencySamples() == 0);

    bool codec = false;
    for (const auto& c : services::store::registeredCodecs())
        codec = codec || c->info().id == "contract-null";
    CHECK(codec);
    CHECK(services::store::registeredMigrations().size() >= 1);

    bool source = false;
    for (const auto& e : io::ControlSourceRegistry::builtin().all())
        source = source || e.id == "contract-virtual";
    CHECK(source);

    // the built-in backends are registered too (soft/juce on every build, gl-* on the device)
    CHECK(ui::render::findRenderer("soft") != nullptr);
    CHECK(ui::render::findRenderer("juce") != nullptr);
    CHECK_FALSE(ui::render::needsChildProbe("soft"));
    std::string error;
    CHECK(ui::render::create("contract-null", {}, error) == nullptr);
    CHECK(error == "contract renderer never draws");
    CHECK(ui::render::create("no-such-backend", {}, error) == nullptr);

    CHECK(ui::ModuleRegistry::builtin().find("contract.module")->opens == "contract.child");
    std::vector<ui::ListItem> items;
    ui::ListSourceRegistry::builtin().find("contract.list")("", items);
    CHECK(items.size() == 1);
    CHECK(ui::PopupRegistry::builtin().find("contract.toast")->kind == ui::PopupSpec::Kind::Toast);
    const auto rows = ui::SettingsRegistry::builtin().rows("utility", "contract");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].label == "VERSION"); // order key, not registration order
    const auto sections = ui::SettingsRegistry::builtin().sections("utility"); // + the screens' own
    CHECK(std::count_if(sections.begin(), sections.end(), [](const auto& s) { return s.id == "contract"; }) == 1);

    // platforms: host is built into host builds
    const auto platforms = platform::registeredPlatforms();
    CHECK(std::find(platforms.begin(), platforms.end(), "host") != platforms.end());
}

TEST_CASE("ScreenRegistry::validate reports broken descriptors", "[contracts][registry]")
{
    ui::ScreenRegistry r;
    static constexpr ui::RailItemSpec rail[] = {{ui::RailItemSpec::Kind::Button, "A", "missing"},
                                                {ui::RailItemSpec::Kind::Button, "B"},
                                                {ui::RailItemSpec::Kind::Button, "C"},
                                                {ui::RailItemSpec::Kind::Button, "D"},
                                                {ui::RailItemSpec::Kind::Button, "E"}};
    r.add({.id = "x", .title = "X", .parent = "nowhere", .body = ui::BodyTemplate::Full,
           .contextBar = ui::ContextBarKind::Menu, .rail = rail, .bodyComponent = "NoSuchWidget",
           .strip = {"X", {"no.such.param"}}});
    r.add({.id = "x", .title = "X2", .parent = "", .body = ui::BodyTemplate::Full, .contextBar = ui::ContextBarKind::Menu});
    const auto problems = r.validate(ui::WidgetRegistry::builtin().dispatcher(), core::builtinParams());
    CHECK(problems.size() == 6); // parent, rail > 4, opens, strip column, body component, duplicate id
}
