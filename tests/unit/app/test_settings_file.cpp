// The app's settings loader (app/SettingsFile.h) — every <PREFIX>_<KEY> variable and every file line
// reaches the settings (not only the defaults' keys), keys lower case, "__" = ".", the environment wins, and the one
// SETTINGS line lists exactly what differs from the defaults.
#include "app/SettingsFile.cpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace
{
const platform::Settings kDefaults {{"render", "auto"}, {"audio", "1"}, {"dev_kit", ""}, {"diag_interval", "10"}};

std::filesystem::path tempFile(const char* name, const std::string& text)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << text;
    return path;
}
} // namespace

TEST_CASE("app settings: keys are normalised", "[app][settings]")
{
    CHECK(app::normaliseSettingsKey("  Input_Trace ") == "input_trace");
    CHECK(app::normaliseSettingsKey("LAMP_LOG") == "lamp_log");
    CHECK(app::normaliseSettingsKey("LIVE__INPUTLATENCY") == "live.inputlatency");
    CHECK(app::normaliseSettingsKey("MIDI_CLOCK__OUT") == "midi_clock.out");
    CHECK(app::normaliseSettingsKey("") == "");
}

TEST_CASE("app settings: file lines — any key, comments, blanks, CRLF", "[app][settings]")
{
    platform::Settings s = kDefaults;
    const int n = app::applySettingsText(s, "# a comment\r\n"
                                            "\n"
                                            "   # indented comment\n"
                                            "Render = gl-fb\r\n"
                                            "input_trace=0\n"
                                            "scale=2\n"
                                            "no equals sign here\n"
                                            "=value without key\n"
                                            "live.inputLatency = 96\n"
                                            "input_script=/root/data/x/hw test/selftest.txt");
    CHECK(n == 5);
    CHECK(s["render"] == "gl-fb");
    CHECK(s["input_trace"] == "0");
    CHECK(s["scale"] == "2");
    CHECK(s["live.inputlatency"] == "96");
    CHECK(s["input_script"] == "/root/data/x/hw test/selftest.txt");
    CHECK(s["audio"] == "1"); // untouched default
    CHECK(s.count("no equals sign here") == 0);
}

TEST_CASE("app settings: every <PREFIX>_<KEY> variable, not only the defaults' keys", "[app][settings]")
{
    platform::Settings s = kDefaults;
    const char* env[] = {"HOME=/root",
                         "APPX_RENDER=soft",
                         "APPX_LAMP_LOG=1",
                         "APPX_INPUT_TRACE=0",
                         "APPX_MIDI_IN=hw:1,0,0",
                         "APPX_LIVE__INPUTLATENCY=128",
                         "APPX_PHONES_DEVICE=none",
                         "APPX_AUDIO_MAP=out34:8/10",
                         "APPX_=ignored",
                         "APPXY_RENDER=other prefix",
                         "APPX_EMPTY=",
                         nullptr};
    CHECK(app::applySettingsEnvironment(s, "APPX", env) == 8);
    CHECK(s["render"] == "soft");
    CHECK(s["lamp_log"] == "1");
    CHECK(s["input_trace"] == "0");
    CHECK(s["midi_in"] == "hw:1,0,0");
    CHECK(s["live.inputlatency"] == "128");
    CHECK(s["phones_device"] == "none");
    CHECK(s["audio_map"] == "out34:8/10");
    CHECK(s["empty"].empty());
    CHECK(s.count("") == 0);
}

TEST_CASE("app settings: defaults, then the file, then the environment", "[app][settings]")
{
    const auto file = tempFile("app-settings-test.conf", "render=gl-fb\nscale=3\ndev_kit=Samples/TestKit\n");
    const char* env[] = {"APPX_RENDER=soft", "APPX_DIAG_INTERVAL=5", nullptr};
    app::SettingsOrigin origin;
    const auto s = app::loadSettings(kDefaults, file.string(), "APPX", env, &origin);
    CHECK(s.at("render") == "soft"); // the environment wins
    CHECK(s.at("scale") == "3");     // a key the defaults do not have, from the file
    CHECK(s.at("dev_kit") == "Samples/TestKit");
    CHECK(s.at("diag_interval") == "5");
    CHECK(s.at("audio") == "1");
    CHECK(origin.fileFound);
    CHECK(origin.fileKeys == 3);
    CHECK(origin.envKeys == 2);
    const std::string line = app::describeSettings(s, kDefaults, origin);
    CHECK(line == "file=" + file.string() + " (read) file_keys=3 env_keys=2 | dev_kit=Samples/TestKit diag_interval=5 "
                                            "render=soft scale=3");
    std::filesystem::remove(file);

    app::SettingsOrigin none;
    const char* noEnv[] = {nullptr};
    const auto d = app::loadSettings(kDefaults, "/nonexistent/dir/x.conf", "APPX", noEnv, &none);
    CHECK(d == kDefaults);
    CHECK_FALSE(none.fileFound);
    CHECK(app::describeSettings(d, kDefaults, none) == "file=/nonexistent/dir/x.conf (missing) file_keys=0 env_keys=0 | "
                                                       "(defaults)");
}

TEST_CASE("app settings: the SETTINGS line quotes values with blanks", "[app][settings]")
{
    platform::Settings s = kDefaults;
    s["input_script"] = "/a b/c.txt";
    s["dev_kit"] = "x";
    s["render"] = ""; // differs from the default "auto": shown as ""
    app::SettingsOrigin o;
    o.file = "/f.conf";
    CHECK(app::describeSettings(s, kDefaults, o)
          == "file=/f.conf (missing) file_keys=0 env_keys=0 | dev_kit=x input_script=\"/a b/c.txt\" render=\"\"");
}

TEST_CASE("app settings: file keys are exported for code that reads its own variables", "[app][settings]")
{
    CHECK(app::settingsEnvironmentName("APPX", "live.inputlatency") == "APPX_LIVE__INPUTLATENCY");
    CHECK(app::settingsEnvironmentName("APPX", "audio_period") == "APPX_AUDIO_PERIOD");
    ::unsetenv("APPXT_AUDIO_PERIOD");
    ::unsetenv("APPXT_LIVE__X");
    ::unsetenv("APPXT_RENDER");
    ::setenv("APPXT_MIDI_IN", "from-env", 1);
    platform::Settings s = kDefaults;
    s["audio_period"] = "512";
    s["live.x"] = "1";
    s["midi_in"] = "from-file";
    const int n = app::exportSettingsToEnvironment(s, kDefaults, "APPXT"); // render=auto is a default: not exported
    CHECK(n == 2);
    CHECK(std::string(std::getenv("APPXT_AUDIO_PERIOD")) == "512");
    CHECK(std::string(std::getenv("APPXT_LIVE__X")) == "1");
    CHECK(std::string(std::getenv("APPXT_MIDI_IN")) == "from-env"); // never overwritten
    CHECK(std::getenv("APPXT_RENDER") == nullptr);
    // The round trip: what was exported reads back as the same keys.
    platform::Settings back;
    const char* env[] = {"APPXT_AUDIO_PERIOD=512", "APPXT_LIVE__X=1", nullptr};
    app::applySettingsEnvironment(back, "APPXT", env);
    CHECK(back.at("audio_period") == "512");
    CHECK(back.at("live.x") == "1");
    for (const char* v : {"APPXT_AUDIO_PERIOD", "APPXT_LIVE__X", "APPXT_MIDI_IN"})
        ::unsetenv(v);
}
