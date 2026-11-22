// The button-role table resources/controllers/panel.json and the compiled tables of platform/imx6/panel/PanelRoles.h
// agree, and together they cover every input control of control-surface.md §2.1 the reference panel reports.
#include "io/controllers/ControlNames.h"
#include "io/controllers/PadCurve.h"
#include "platform/imx6/panel/PanelRoles.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace platform::imx6::panel;

namespace
{
juce::var loadTable()
{
    const juce::File file(juce::String(APP_SOURCE_DIR) + "/resources/controllers/panel.json");
    REQUIRE(file.existsAsFile());
    const auto v = juce::JSON::parse(file.loadFileAsString());
    REQUIRE(v.isObject());
    return v;
}
io::Control control(const juce::Identifier& name)
{
    const auto s = name.toString().toStdString();
    if (s.rfind("bar.position.", 0) == 0)
        return io::barPositionLamp(std::stoi(s.substr(13)));
    const auto c = io::controlFromName(s);
    INFO(s);
    REQUIRE(c.has_value());
    return *c;
}
} // namespace

TEST_CASE("panel roles: panel.json buttons equal the compiled button roles")
{
    const auto table = loadTable();
    const auto* buttons = table["buttons"].getDynamicObject();
    REQUIRE(buttons != nullptr);
    CHECK(buttons->getProperties().size() == static_cast<int>(std::size(kButtons)));
    for (const auto& p : buttons->getProperties())
    {
        const auto c = control(p.name);
        INFO(p.name.toString());
        const auto byte = static_cast<std::size_t>(int(p.value[0])), bit = static_cast<std::size_t>(int(p.value[1]));
        CHECK(kButtonGrid.at[byte][bit] == c);
    }
    std::set<int> bits; // no bit has two roles, system keys included
    for (const auto& b : kButtons)
        CHECK(bits.insert(b.byte * 8 + b.bit).second);
    for (const auto& b : kSystemButtons)
        CHECK(bits.insert(b.byte * 8 + b.bit).second);
    CHECK(bits.size() == 38); // the 37 dispatched button objects of E2 + the selector push
}

TEST_CASE("panel roles: counters, analog words, pads and touch match panel.json")
{
    const auto table = loadTable();
    const auto* counters = table["counters"].getDynamicObject();
    REQUIRE(counters != nullptr);
    CHECK(counters->getProperties().size() == static_cast<int>(std::size(kCounters)));
    for (const auto& k : kCounters)
    {
        const auto v = counters->getProperty(juce::Identifier(std::string(io::nameOf(k.control))));
        CHECK(int(v[0]) == k.offset);
        CHECK(int(v[1]) == k.sign);
    }
    const auto* analog = table["analog"].getDynamicObject();
    REQUIRE(analog != nullptr);
    for (const auto& a : kAnalog)
    {
        const auto v = analog->getProperty(juce::Identifier(std::string(io::nameOf(a.control))));
        CHECK(int(v[0]) == a.offset);
        CHECK(bool(v[1]) == a.inverted);
    }
    const auto pads = table["pads"];
    CHECK(int(pads["pressure_offset"]) == kPadPressureOffset);
    CHECK(int(pads["contact_offset"]) == kPadContactOffset);
    const io::PadCalibration cal;
    CHECK(int(pads["contact_min"]) == cal.contactMin);
    CHECK(int(pads["threshold"]) == cal.threshold);
    CHECK(int(pads["floor"]) == cal.floor);
    CHECK(int(pads["ceiling"]) == cal.ceiling);
    const auto touch = table["touch"];
    const TouchCalibration t;
    CHECK(int(touch["x_offset"]) == kTouchXOffset);
    CHECK(int(touch["y_offset"]) == kTouchYOffset);
    CHECK((int(touch["raw_x"][0]) == t.rawLeft && int(touch["raw_x"][1]) == t.rawRight));
    CHECK((int(touch["raw_y"][0]) == t.rawTop && int(touch["raw_y"][1]) == t.rawBottom));
    CHECK((int(touch["contact"][0]) == t.minX && int(touch["contact"][3]) == t.maxY));
}

TEST_CASE("panel roles: lamp channels match panel.json, mono layout included")
{
    const auto table = loadTable();
    const auto* lamps = table["lamps"].getDynamicObject();
    REQUIRE(lamps != nullptr);
    CHECK(lamps->getProperties().size() == static_cast<int>(kLamps.size()));
    for (const auto& p : lamps->getProperties())
    {
        const auto c = control(p.name);
        INFO(p.name.toString());
        bool found = false;
        for (const auto& l : kLamps)
            if (l.control == c)
            {
                found = true;
                CHECK(l.rgb == (p.value[0].toString() == "rgb"));
                CHECK(int(l.channel) == int(p.value[1]));
            }
        CHECK(found);
    }
    const auto* mono = table["tx"]["mono"].getDynamicObject();
    REQUIRE(mono != nullptr);
    CHECK(mono->getProperties().size() == static_cast<int>(std::size(kMono)));
    for (int ch = 0; ch < static_cast<int>(std::size(kMono)); ++ch)
    {
        const auto v = mono->getProperty(juce::Identifier(juce::String(ch)));
        CHECK(int(v[0]) == kMono[ch].byte);
        CHECK(int(v[1]) == kMono[ch].bit);
        CHECK((v[2].isVoid() ? 0 : int(v[2])) == kMono[ch].flagByte);
    }
}

TEST_CASE("controls: every §2.1 input control has a source on the imx6 platform, except the two pure-analog pots")
{
    std::set<io::Control> covered;
    for (int i = 1; i <= 16; ++i)
        covered.insert(io::pad(i));
    for (const auto& b : kButtons)
        covered.insert(b.control);
    for (const auto& k : kCounters)
        covered.insert(k.control);
    for (const auto& a : kAnalog)
        covered.insert(a.control);
    covered.insert(io::Control::Screen); // LCD touch words in the panel frame
    covered.insert(io::Control::Strip);  // /dev/cdc_spi2.0
    const auto table = loadTable();
    std::set<io::Control> noField;
    for (const auto& v : *table["no_wire_field"].getArray())
        noField.insert(*io::controlFromName(v.toString().toStdString()));
    CHECK(noField == std::set {io::Control::AfsCutoff, io::Control::AfsResonance});
    for (const auto c : io::referenceInputControls())
    {
        INFO(io::nameOf(c));
        CHECK((covered.count(c) == 1) != (noField.count(c) == 1));
    }
}
