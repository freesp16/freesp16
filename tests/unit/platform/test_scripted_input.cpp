// The scripted input source (io/controllers/ScriptedInput.h): the line format, its errors, the order of the
// steps, and the replay timing (time 0 = the first poll), pads to the TriggerSink, quit and screenshot requests.
#include "io/controllers/AppRequests.h"
#include "io/controllers/ScriptedInput.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using Kind = io::ControlEvent::Kind;

namespace
{
struct Sink final : engine::TriggerSink
{
    std::vector<engine::TriggerNow> hits;
    bool push(const engine::TriggerNow& t) noexcept override
    {
        hits.push_back(t);
        return true;
    }
};
} // namespace

TEST_CASE("platform script: parsing the line format", "[platform][script]")
{
    const auto r = io::parseInputScript("# a comment\n"
                                        "\n"
                                        "500 press pad5 110   # trailing comment\n"
                                        "100 turn knob1 -20\n"
                                        "900 release pad.5\n"
                                        "1000 touch 400 240\n"
                                        "1100 drag 410 250\n"
                                        "1200 release-touch\n"
                                        "1300 tap 10 20\n"
                                        "1400 value afs.cutoff 64\n"
                                        "1500 press Play\n"
                                        "2000 screenshot out/home.png\n"
                                        "3000 quit\n");
    INFO((r.errors.empty() ? std::string() : r.errors.front()));
    REQUIRE(r.errors.empty());
    REQUIRE(r.steps.size() == 12);
    const auto& s = r.steps;
    CHECK(s[0].atMs == 100); // sorted by time
    CHECK(s[0].event.kind == Kind::Turn);
    CHECK(s[0].event.control == io::Control::Knob1);
    CHECK(s[0].event.value == -20);
    CHECK(s[1].event.kind == Kind::Press);
    CHECK(s[1].event.control == io::pad(5));
    CHECK(s[1].event.value == 110);
    CHECK(s[2].event.kind == Kind::Release);
    CHECK(s[2].event.control == io::pad(5));
    CHECK((s[3].event.kind == Kind::Touch && s[3].event.control == io::Control::Screen && s[3].event.value == 1));
    CHECK((s[3].event.x == 400 && s[3].event.y == 240));
    CHECK((s[4].event.value == 2 && s[4].event.x == 410 && s[4].event.y == 250));
    CHECK((s[5].event.value == 0 && s[5].event.x == 410 && s[5].event.y == 250)); // the last touch point
    CHECK((s[6].atMs == 1300 && s[6].event.value == 1 && s[6].event.x == 10 && s[6].event.y == 20));
    CHECK((s[7].atMs == 1350 && s[7].event.value == 0 && s[7].event.x == 10)); // tap = touch + release 50 ms later
    CHECK((s[8].event.kind == Kind::Value && s[8].event.control == io::Control::AfsCutoff && s[8].event.value == 64));
    CHECK((s[9].event.control == io::Control::Play && s[9].event.value == 100)); // default velocity
    CHECK((s[10].op == io::ScriptStep::Op::Screenshot && s[10].arg == "out/home.png"));
    CHECK(s[11].op == io::ScriptStep::Op::Quit);

    CHECK(io::scriptControl("knob3") == io::Control::Knob3);
    CHECK(io::scriptControl("enc.6") == io::Control::Knob6);
    CHECK(io::scriptControl("step16") == io::step(16));
    CHECK(io::scriptControl("sel.push") == io::Control::SelectorPush);
    CHECK_FALSE(io::scriptControl("pad17"));
    CHECK_FALSE(io::scriptControl("knob"));
}

TEST_CASE("platform script: errors name the line", "[platform][script]")
{
    const auto r =
        io::parseInputScript("10 press pad17\nabc quit\n20 turn knob1\n30 touch 5\n40 jump\n50\n60 press pad1 x\n");
    REQUIRE(r.errors.size() == 7);
    CHECK(r.errors[0] == "line 1: unknown control 'pad17'");
    CHECK(r.errors[1].rfind("line 2: bad time", 0) == 0);
    CHECK(r.errors[2].rfind("line 3: turn needs a value", 0) == 0);
    CHECK(r.errors[3].rfind("line 4: touch needs", 0) == 0);
    CHECK(r.errors[4] == "line 5: unknown command 'jump'");
    CHECK(r.errors[5] == "line 6: missing command");
    CHECK(r.errors[6].rfind("line 7: bad value", 0) == 0);

    io::ScriptedInputSource source(nullptr, r, "test.txt");
    std::string info;
    CHECK_FALSE(source.start(info)); // a broken script replays nothing and says why
    CHECK(info.rfind("test.txt: line 1: unknown control 'pad17' (+6 more)", 0) == 0);
    std::vector<io::ControlEvent> out;
    source.pollAt(1000, out);
    source.pollAt(100000, out);
    CHECK(out.empty());
}

TEST_CASE("platform script: replay timing, pads to the trigger sink, quit and screenshot", "[platform][script]")
{
    io::clearQuitRequest();
    Sink sink;
    io::ScriptedInputSource source(&sink, io::parseInputScript("0 press pad5 110\n"
                                                               "100 release pad5\n"
                                                               "250 turn knob1 3\n"
                                                               "300 screenshot a.png\n"
                                                               "400 quit\n"),
                                   "t");
    std::string info;
    REQUIRE(source.start(info));
    CHECK(info == "t: 5 steps over 400 ms (time 0 = first poll)");
    std::vector<io::ControlEvent> out;
    source.pollAt(5000.0, out); // time 0 = this first poll: only the step at 0 ms is due
    REQUIRE(out.size() == 1);
    CHECK((out[0].kind == Kind::Press && out[0].control == io::pad(5) && out[0].value == 110));
    CHECK(out[0].timeMs == 5000.0);
    REQUIRE(sink.hits.size() == 1); // UI-118: the pad reaches the engine directly
    CHECK((sink.hits[0].track == 4 && sink.hits[0].velocity == 110 && sink.hits[0].pad == 4));
    out.clear();
    source.pollAt(5099.0, out);
    CHECK(out.empty());
    source.pollAt(5100.0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == Kind::Release);
    out.clear();
    source.pollAt(5350.0, out); // a late poll delivers everything due, in order
    REQUIRE(out.size() == 1);
    CHECK((out[0].kind == Kind::Turn && out[0].value == 3));
    std::string shot;
    REQUIRE(io::takeScreenshotRequest(shot));
    CHECK(shot == "a.png");
    CHECK_FALSE(io::takeScreenshotRequest(shot));
    CHECK_FALSE(io::quitRequested());
    CHECK_FALSE(source.finished());
    source.pollAt(5400.0, out);
    CHECK(io::quitRequested());
    CHECK(source.finished());
    CHECK(sink.hits.size() == 1); // releases and turns never trigger
    io::clearQuitRequest();
}

TEST_CASE("platform script: standby asks the app to power off", "[platform][script]")
{
    io::clearQuitRequest();
    io::clearStandbyRequest();
    Sink sink;
    io::ScriptedInputSource source(&sink, io::parseInputScript("100 standby\n"), "t");
    std::string info;
    REQUIRE(source.start(info));
    std::vector<io::ControlEvent> out;
    source.pollAt(0.0, out);
    CHECK_FALSE(io::standbyRequested());
    source.pollAt(100.0, out);
    CHECK(io::standbyRequested());
    CHECK_FALSE(io::quitRequested()); // standby is its own request: the app powers off after quitting
    CHECK(out.empty());
    io::clearStandbyRequest();
}

TEST_CASE("platform script: the registered source exists only with a configured path", "[platform][script]")
{
    const io::ControlSourceRegistry::Entry* entry = nullptr;
    for (const auto& e : io::ControlSourceRegistry::builtin().all())
        if (e.id == "script")
            entry = &e;
    REQUIRE(entry != nullptr);
    io::setInputScriptPath("");
    CHECK(entry->create(nullptr) == nullptr);
    io::setInputScriptPath("/nonexistent/script.txt");
    auto source = entry->create(nullptr);
    REQUIRE(source != nullptr);
    std::string info;
    CHECK_FALSE(source->start(info));
    CHECK(info.find("cannot read") != std::string::npos);
    io::setInputScriptPath("");
}

TEST_CASE("platform script: app commands; a pad sent direct is marked as a panel press", "[platform][script]")
{
    Sink sink;
    CHECK(io::parseInputScript("20 cmd\n").errors.size() == 1); // "cmd" without a command
    io::ScriptedInputSource source(&sink, io::parseInputScript("0 press pad2 90\n10 cmd   scene-bpm  2 100\n"), "t");
    std::string info;
    REQUIRE(source.start(info));
    std::vector<io::ControlEvent> out;
    source.pollAt(0.0, out);
    source.pollAt(50.0, out);
    REQUIRE(out.size() == 1);
    // The pad already reached the TriggerSink: the panel source tells the pad controller not to post it again.
    CHECK(out[0].source == io::kSourcePanel);
    CHECK(sink.hits.size() == 1);
    std::string text;
    REQUIRE(io::takeAppCommand(text));
    CHECK(text == "scene-bpm 2 100");
    CHECK_FALSE(io::takeAppCommand(text));
}
