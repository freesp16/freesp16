// MIDI learn (MID-034 / FX-088; services/midilearn/MidiLearn.h): targets include the Send /
// Master FX parameters, learn capture refuses controller ports, bindings map onto their range, survive the project
// file, and a learned CC travels through the engine's CC overlay to the UI and onto its target.
#include "tests/unit/midi/MidiTestUtil.h"

#include "core/model/ModelOps.h"
#include "services/midilearn/MidiLearn.h"
#include "services/store/CodecRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ml = services::midilearn;

namespace
{
const core::ParamSpec* firstWithPrefix(std::string_view prefix)
{
    for (const auto& s : core::builtinParams().all())
        if (s.id.substr(0, prefix.size()) == prefix && s.type == core::ParamType::Continuous)
            return &s;
    return nullptr;
}

ml::LearnBinding cc(int channel, int number, std::string_view target, std::uint16_t scope, float lo = 0, float hi = 1)
{
    ml::LearnBinding b;
    b.channel = static_cast<std::uint8_t>(channel);
    b.status = ml::kStatusCc;
    b.number = static_cast<std::uint8_t>(number);
    b.target = miditest::id(target);
    b.scope = scope;
    b.lo = lo;
    b.hi = hi;
    return b;
}

int freeCc(const core::midi::CcTable& t)
{
    for (int c = 1; c < 128; ++c)
        if (! ml::fixedCc(t, c))
            return c;
    return -1;
}
} // namespace

TEST_CASE("ext midilearn: any user parameter path is a target, Send / Master FX included (FX-088)", "[ext][midilearn]")
{
    const auto& r = core::builtinParams();
    const auto targets = ml::learnTargets(r);
    auto has = [&](const core::ParamSpec* s) {
        return s != nullptr && std::find(targets.begin(), targets.end(), *r.idOf(s->id)) != targets.end();
    };
    const auto* send = firstWithPrefix("send.");
    const auto* master = firstWithPrefix("master.");
    REQUIRE(send != nullptr);
    REQUIRE(master != nullptr);
    CHECK(has(send));
    CHECK(has(master));
    CHECK_FALSE(send->lockable); // FX-094: a learned CC sets the stored value only
    CHECK(has(r.find("track.volume")));
    CHECK(has(r.find("playback.pitch")));
    CHECK_FALSE(has(r.find("ext.midi.learn.cc"))); // the carrier
    CHECK_FALSE(has(r.find("afs.ping")));          // internal
    for (const auto id : targets)
        CHECK_FALSE(r[id].isPreference()); // bindings are per project
}

TEST_CASE("ext midilearn: learn captures the next CC or note, never from a controller port", "[ext][midilearn]")
{
    ml::LearnSession s;
    CHECK_FALSE(s.capture({1, ml::kStatusCc, 20, 64}, false));
    s.arm(miditest::id("track.pan"), 0x23, 0.25f, 0.75f);
    CHECK(s.armed());
    CHECK_FALSE(s.capture({1, ml::kStatusCc, 20, 64}, true)); // a `controller` port (the controller hub's)
    CHECK(s.armed());
    CHECK_FALSE(s.capture({1, 0x80, 60, 0}, false)); // a note off is not a gesture
    const auto b = s.capture({5, ml::kStatusCc, 21, 64}, false);
    REQUIRE(b);
    CHECK(b->channel == 5);
    CHECK(b->number == 21);
    CHECK(b->target == miditest::id("track.pan"));
    CHECK(b->scope == 0x23);
    CHECK(b->lo == 0.25f);
    CHECK_FALSE(s.armed());
    s.arm(miditest::id("track.mute"), 0);
    const auto n = s.capture({2, ml::kStatusNote, 36, 100}, false);
    REQUIRE(n);
    CHECK(n->status == ml::kStatusNote);
}

TEST_CASE("ext midilearn: bindings map CC / notes onto their range, after (never over) the fixed map",
          "[ext][midilearn]")
{
    const auto& r = core::builtinParams();
    const auto& fixed = miditest::table();
    const int c = freeCc(fixed);
    REQUIRE(c > 0);
    CHECK_FALSE(ml::fixedCc(fixed, 85)); // the CC scripts/smoke/99-ext.sh learns
    ml::LearnMap map;
    REQUIRE(map.add(cc(1, c, "track.volume", 0x02), r, &fixed));
    CHECK_FALSE(map.add(cc(1, 7, "track.pan", 0), r, &fixed)); // CC 7 = the fixed map's volume
    CHECK_FALSE(map.add(cc(1, c, "ext.midi.learn.cc", 0), r));
    CHECK_FALSE(map.add(cc(1, c, "track.volume", 0, -0.5f, 1.0f), r));
    const auto* send = firstWithPrefix("send.");
    REQUIRE(map.add(cc(3, c, std::string(send->id), 0x10, 0.5f, 1.0f), r, &fixed));
    ml::LearnBinding note;
    note.channel = 1;
    note.status = ml::kStatusNote;
    note.number = 40;
    note.target = miditest::id("track.mute");
    note.scope = 0x05;
    REQUIRE(map.add(note, r));
    CHECK(map.bindings().size() == 3);

    std::vector<ml::Applied> out;
    CHECK(map.apply({1, ml::kStatusCc, static_cast<std::uint8_t>(c), 127}, r, out) == 1);
    CHECK(map.apply({1, ml::kStatusCc, static_cast<std::uint8_t>(c), 0}, r, out) == 1);
    CHECK(map.apply({1, ml::kStatusCc, static_cast<std::uint8_t>(c), 64}, r, out) == 1);
    REQUIRE(out.size() == 3);
    CHECK(out[0].value == 127.0f); // track.volume 0–127, integral
    CHECK(out[1].value == 0.0f);
    CHECK(out[2].value == 64.0f);
    CHECK(out[0].scope == 0x02);
    out.clear();
    CHECK(map.apply({3, ml::kStatusCc, static_cast<std::uint8_t>(c), 0}, r, out) == 1); // lo = 0.5 of the range
    CHECK(out[0].value == send->minValue + 0.5f * (send->maxValue - send->minValue));
    out.clear();
    CHECK(map.apply({2, ml::kStatusCc, static_cast<std::uint8_t>(c), 0}, r, out) == 0); // another channel
    CHECK(map.apply({1, ml::kStatusNote, 40, 90}, r, out) == 1);
    CHECK(map.apply({1, 0x80, 40, 0}, r, out) == 1);
    CHECK(map.apply({1, ml::kStatusNote, 40, 0}, r, out) == 1); // note on, velocity 0 = off
    CHECK(out[0].value == 1.0f);
    CHECK(out[1].value == 0.0f);
    CHECK(out[2].value == 0.0f);
    // The same source replaces its binding; remove drops it.
    REQUIRE(map.add(cc(1, c, "track.pan", 0x02), r, &fixed));
    CHECK(map.bindings().size() == 3);
    CHECK(map.remove(1, ml::kStatusCc, static_cast<std::uint8_t>(c)));
    CHECK(map.bindings().size() == 2);
}

TEST_CASE("ext midilearn: bindings are stored per project and survive the native file with the flag off",
          "[ext][midilearn]")
{
    const auto& r = core::builtinParams();
    ml::LearnMap map;
    REQUIRE(map.add(cc(2, 85, "track.pan", 0x31, 0.1f, 0.9f), r));
    REQUIRE(map.add(cc(16, 86, std::string(firstWithPrefix("master.")->id), 4), r));
    const auto json = map.toJson(r);
    auto back = ml::LearnMap::fromJson(json, r);
    REQUIRE(back);
    CHECK(back.value() == map);

    auto p = core::model::newDefaultProject();
    p->extras.set(std::string(ml::kExtrasKey), json, core::model::Extras::Encoding::Json);
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("ext-midilearn-" + juce::String(juce::Time::currentTimeMillis()));
    const auto bundle = dir.getChildFile("Learn.sixteen");
    REQUIRE(dir.createDirectory());
    REQUIRE(services::store::saveProject(*p, bundle));
    auto q = services::store::openProject(bundle);
    REQUIRE(q);
    CHECK(ml::LearnMap::load(*q.value(), r) == map);
    CHECK(q.value()->extras == p->extras);
    // An unknown target in a stored map is dropped with a warning, the rest kept.
    std::string warn;
    auto partial = ml::LearnMap::fromJson(R"([{"ch":1,"st":176,"no":90,"param":"no.such","scope":0,"lo":0,"hi":1}])",
                                          r, &warn);
    REQUIRE(partial);
    CHECK(partial.value().empty());
    CHECK_FALSE(warn.empty());
    dir.deleteRecursively();
}

TEST_CASE("ext midilearn: a learned CC goes through the engine's CC overlay to the UI and onto its target",
          "[ext][midilearn]")
{
    auto e = miditest::make();
    const auto& fixed = miditest::table();
    const auto carrier = miditest::id(ml::kCarrierId);
    REQUIRE(carrier.valid());
    core::midi::CcTable overlay;
    ml::buildOverlay(fixed, carrier, overlay);
    for (int c = 0; c < 128; ++c) // the fixed entries are untouched
        if (ml::fixedCc(fixed, c))
            CHECK(overlay.cc[static_cast<std::size_t>(c)].param == fixed.cc[static_cast<std::size_t>(c)].param);
    e->midi().hub().setCcTable(&overlay);
    miditest::Driver d {*e, 64};
    core::midi::UiEvent u;
    while (e->midi().hub().popUi(u))
    {
    }
    const int c = freeCc(fixed);
    REQUIRE(c > 0);
    // Channel 3 = track 3's receive channel by default; CC c = 100.
    REQUIRE(e->midi().hub().pushIn(0, miditest::msg(0xB2, static_cast<std::uint8_t>(c), 100,
                                                    miditest::hostNs(static_cast<double>(d.frame()) - 1))));
    d.blocks(2);
    std::vector<core::midi::UiEvent> ui;
    while (e->midi().hub().popUi(u))
        ui.push_back(u);
    const auto it = std::find_if(ui.begin(), ui.end(), [&](const auto& x) { return x.param == carrier.value; });
    REQUIRE(it != ui.end());
    auto project = core::model::newDefaultProject();
    const auto m = ml::decodeCarrier(*it, carrier, project->midi);
    REQUIRE(m);
    CHECK(m->channel == 3);
    CHECK(m->number == c);
    CHECK(m->value == 100);

    const auto* send = firstWithPrefix("send.");
    ml::LearnMap map;
    REQUIRE(map.add(cc(3, c, std::string(send->id), 0), core::builtinParams(), &fixed));
    std::vector<ml::Applied> out;
    CHECK(map.apply(*m, core::builtinParams(), out) == 1);
    CHECK(out[0].spec == send);
    CHECK(out[0].value == send->minValue + (100.0f / 127.0f) * (send->maxValue - send->minValue));
    e->midi().hub().setCcTable(&fixed);
}
