// Controller profiles (control-surface.md UI-130/131/134): every JSON profile in resources/controllers/
// validates, the compiled-in text is the file byte for byte, the built-ins say what UI-131 / UI-134 say, the validator
// names what is wrong, toJson round-trips, and port patterns pick the profile.
#include "io/controllers/profiles/Json.h"
#include "io/controllers/profiles/Profile.h"

#include <catch2/catch_test_macros.hpp>

#include <dirent.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ctl = io::controllers;
using io::Control;

namespace
{
std::string read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<std::string> profileFiles()
{
    std::vector<std::string> out;
    const std::string dir = std::string(APP_SOURCE_DIR) + "/resources/controllers";
    if (DIR* d = opendir(dir.c_str()))
    {
        while (const dirent* e = readdir(d))
        {
            const std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 && n != "panel.json") // panel.json: imx6 wiring
                out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

const ctl::MidiBinding* find(const ctl::LoadedProfile& p, std::uint8_t st, std::uint8_t number, std::uint8_t ch = 255)
{
    for (const auto& b : p.in)
        if (b.status == st && b.number == number && (ch == 255 || b.channel == ch))
            return &b;
    return nullptr;
}

std::string minimal(const std::string& in, const std::string& extra = "")
{
    return R"({"schema": 1, "id": "t", "name": "T", "channel": 1)" + extra + R"(, "in": [)" + in + "]}";
}
} // namespace

TEST_CASE("test_controller_profiles: every JSON profile in resources/controllers validates, equals its "
          "compiled-in text",
          "[controllers]")
{
    const auto files = profileFiles();
    REQUIRE(files.size() >= 2);
    for (const auto& f : files)
    {
        INFO(f);
        const auto r = ctl::parseProfile(read(std::string(APP_SOURCE_DIR) + "/resources/controllers/" + f));
        CHECK(r.ok());
        if (! r)
            UNSCOPED_INFO(r.error().detail);
    }
    REQUIRE(ctl::builtinProfileTexts().size() == 2);
    for (const auto& t : ctl::builtinProfileTexts())
    {
        INFO(t.file);
        CHECK(std::string(t.json) == read(std::string(APP_SOURCE_DIR) + "/resources/controllers/" + std::string(t.file)));
    }
    REQUIRE(ctl::builtinProfiles().size() == 2);
    CHECK(ctl::builtinProfiles()[0].builtin);
}

TEST_CASE("Generic 16 + 8 is UI-131 (pads 36-51 ch 10, encoders, transport, shift, strip)", "[controllers]")
{
    const auto& g = ctl::builtinProfiles()[0];
    REQUIRE(g.id == ctl::kGenericProfileId);
    CHECK(g.fallback());
    CHECK(g.channel == 10);
    CHECK(g.padBank);
    for (int n = 0; n < 16; ++n)
    {
        const auto* b = find(g, ctl::status::note, static_cast<std::uint8_t>(36 + n));
        REQUIRE(b != nullptr);
        CHECK(b->control == io::pad(n + 1));
        CHECK(b->channel == 10);
        CHECK(b->aftertouch);
    }
    for (int n = 0; n < 6; ++n)
    {
        const auto* b = find(g, ctl::status::cc, static_cast<std::uint8_t>(70 + n));
        REQUIRE(b != nullptr);
        CHECK(b->control == io::knob(n + 1));
        CHECK(b->enc == ctl::Encoding::TwosComplement);
    }
    CHECK(find(g, ctl::status::cc, 76)->control == Control::Selector);
    CHECK(find(g, ctl::status::cc, 77)->control == Control::MainVolume);
    CHECK(find(g, ctl::status::cc, 77)->scale == 8);
    CHECK(find(g, ctl::status::note, 52)->control == Control::SelectorPush);
    CHECK(find(g, ctl::status::note, 53)->control == Control::Back);
    CHECK(find(g, ctl::status::note, 54)->control == Control::Home);
    CHECK(find(g, ctl::status::mmc, 0x02)->control == Control::Play);
    CHECK(find(g, ctl::status::mmc, 0x01)->control == Control::Stop);
    CHECK(find(g, ctl::status::mmc, 0x06)->control == Control::Rec);
    CHECK(find(g, ctl::status::cc, 115)->control == Control::Play);
    CHECK(find(g, ctl::status::cc, 116)->control == Control::Stop);
    CHECK(find(g, ctl::status::cc, 117)->control == Control::Rec);
    CHECK(find(g, ctl::status::cc, 118)->control == Control::Shift);
    CHECK(find(g, ctl::status::pitchBend, 0)->control == Control::Strip);
    CHECK(find(g, ctl::status::cc, 119)->enc == ctl::Encoding::Contact);
    CHECK(find(g, ctl::status::note, 55)->function == ctl::Function::PadBank);
    CHECK(g.lamps.size() == 19); // 16 pads + PLAY / STOP / REC
}

TEST_CASE("Reference sampler MIDI OUT is UI-134 (pads from 60, CC1 / CC2 pages, start / stop)",
          "[controllers]")
{
    const auto& r = ctl::builtinProfiles()[1];
    REQUIRE(r.id == ctl::kReferenceProfileId);
    CHECK(r.portPattern.empty()); // chosen by hand
    for (int n = 0; n < 16; ++n)
        CHECK(find(r, ctl::status::note, static_cast<std::uint8_t>(60 + n), 0)->control == io::pad(n + 1));
    CHECK(find(r, ctl::status::pitchBend, 0)->control == io::knob(1));
    CHECK(find(r, ctl::status::pressure, 0)->control == io::knob(2));
    const std::uint8_t cc1[] = {7, 1, 2, 10};
    for (int i = 0; i < 4; ++i)
        CHECK(find(r, ctl::status::cc, cc1[i])->control == io::knob(i + 3));
    for (int i = 0; i < 6; ++i)
    {
        const auto* b = find(r, ctl::status::cc, static_cast<std::uint8_t>(71 + i));
        CHECK(b->control == io::knob(i + 1));
        CHECK(b->enc == ctl::Encoding::Absolute);
    }
    CHECK(find(r, ctl::status::realtime, 0xFA)->control == Control::Play);
    CHECK(find(r, ctl::status::realtime, 0xFB)->control == Control::Play);
    CHECK(find(r, ctl::status::realtime, 0xFC)->control == Control::Stop);
    CHECK(r.lamps.empty());
}

TEST_CASE("the validator names the problem", "[controllers]")
{
    auto bad = [](const std::string& text) {
        const auto r = ctl::parseProfile(text);
        return r.ok() ? std::string() : r.error().detail;
    };
    CHECK(bad("{") .find("json") == 0);
    CHECK(bad(R"({"schema": 2, "id": "x", "in": []})").find("schema") == 0);
    CHECK(bad(R"({"schema": 1, "id": "a b", "in": []})").find("id") == 0);
    CHECK(bad(minimal(R"({"note": 36, "control": "pad.17"})")).find("unknown control") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "control": "pad.1"}, {"note": 36, "control": "pad.2"})"))
              .find("same message") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "count": 4, "control": "pad.14"})")).find("past the control group")
          != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "control": "enc.1"})")).find("cannot drive") != std::string::npos);
    CHECK(bad(minimal(R"({"cc": 1, "enc": "twos", "control": "play"})")).find("cannot drive") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 1, "enc": "twos", "control": "enc.1"})")).find("does not suit") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "cc": 3, "control": "pad.1"})")).find("exactly one") != std::string::npos);
    CHECK(bad(minimal(R"({"cc": 3, "aftertouch": true, "control": "pad.1"})")).find("aftertouch") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 55, "control": "ext.steps"})")).find("padBank") == 0);
    CHECK(bad(minimal(R"({"note": 36, "control": "pad.1"})", R"(, "lamps": [{"control": "pad.1", "note": 36, "palette": "x"}])"))
              .find("unknown palette") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "control": "pad.1"})",
                      R"(, "palettes": {"p": ["00000"]})")).find("RRGGBB") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "control": "pad.1"})", R"(, "palettes": {"p": ["000000"]}, "lamps": [{"control": "enc.1", "note": 1, "palette": "p"}])"))
              .find("has no lamp") != std::string::npos);
    CHECK(bad(minimal(R"({"note": 36, "control": "pad.1"})")).empty());
}

TEST_CASE("toJson round-trips every built-in and a learned profile", "[controllers]")
{
    for (const auto& p : ctl::builtinProfiles())
    {
        INFO(p.id);
        const auto again = ctl::parseProfile(ctl::toJson(p));
        REQUIRE(again.ok());
        const auto& q = again.value();
        CHECK(q.id == p.id);
        CHECK(q.name == p.name);
        CHECK(q.portPattern == p.portPattern);
        CHECK(q.channel == p.channel);
        CHECK(q.padBank == p.padBank);
        REQUIRE(q.in.size() == p.in.size());
        for (std::size_t i = 0; i < p.in.size(); ++i)
        {
            CHECK(q.in[i].status == p.in[i].status);
            CHECK(q.in[i].channel == p.in[i].channel);
            CHECK(q.in[i].number == p.in[i].number);
            CHECK(q.in[i].enc == p.in[i].enc);
            CHECK(q.in[i].control == p.in[i].control);
            CHECK(q.in[i].scale == p.in[i].scale);
            CHECK(q.in[i].aftertouch == p.in[i].aftertouch);
            CHECK(q.in[i].function == p.in[i].function);
        }
        CHECK(q.palettes == p.palettes);
        REQUIRE(q.lamps.size() == p.lamps.size());
        for (std::size_t i = 0; i < p.lamps.size(); ++i)
        {
            CHECK(q.lamps[i].control == p.lamps[i].control);
            CHECK(q.lamps[i].number == p.lamps[i].number);
            CHECK(q.lamps[i].status == p.lamps[i].status);
        }
    }
    // A mono palette's explicit values survive ("FFFFFF:127").
    const auto& g = ctl::builtinProfiles()[0];
    CHECK((g.palettes[1][1] >> 24) == 127);
    CHECK((g.palettes[1][1] & 0xFFFFFFu) == 0xFFFFFFu);
}

TEST_CASE("port patterns pick the profile, Generic 16 + 8 is the fallback", "[controllers]")
{
    CHECK(ctl::matchesPort("*", "anything"));
    CHECK(ctl::matchesPort("pad*ctl", "PAD 16 CTL"));
    CHECK(ctl::matchesPort("foo|*bar?", "my bar1"));
    CHECK_FALSE(ctl::matchesPort("foo|*bar?", "my bar"));
    CHECK_FALSE(ctl::matchesPort("", "x"));
    std::vector<ctl::LoadedProfile> ps = ctl::builtinProfiles();
    auto special = ctl::parseProfile(R"({"schema": 1, "id": "special", "ports": "Special Pads*", "in": []})");
    REQUIRE(special.ok());
    ps.push_back(special.value());
    CHECK(ctl::profileForPort(ps, "Special Pads MIDI 1")->id == "special");
    CHECK(ctl::profileForPort(ps, "Some Controller")->id == ctl::kGenericProfileId);
}

TEST_CASE("the JSON reader handles escapes, nesting and errors", "[controllers]")
{
    auto v = ctl::json::parse(R"({"a": "x\"é😀", "b": [1, -2.5e1, true, null], "a": 3})");
    REQUIRE(v.ok());
    CHECK(v.value().get("a")->string == "x\"\xC3\xA9\xF0\x9F\x98\x80"); // the first "a" wins
    CHECK(v.value().get("b")->items[1].number == -25.0);
    CHECK(v.value().get("b")->items[2].boolean);
    CHECK_FALSE(ctl::json::parse("[1,]").ok());
    CHECK_FALSE(ctl::json::parse(R"({"a" 1})").ok());
    CHECK_FALSE(ctl::json::parse("1 2").ok());
    CHECK(ctl::json::quote("a\"b\n") == "\"a\\\"b\\n\"");
}
