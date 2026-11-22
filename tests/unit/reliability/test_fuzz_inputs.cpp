// NFR-011 "no crash on malformed input files": deterministic mutation fuzzing of the untrusted inputs the
// earlier fuzz suites do not reach. Covered elsewhere: WAV / AIFF / AIFC, .wav.dat + .wav.json sidecars (tests/unit/
// library/test_fuzz.cpp), the project bundle files, autosave copies and damaged bundles on disk (tests/unit/project/
// test_fuzz.cpp), the reference .prj container, trees, steps and .wav.dat (tests/unit/interop/test_fuzz.cpp).
// Here, each from a valid generated input (no device data) with bytes flipped, cut, extended, tokens spliced:
//   preferences.json            services::settings::Preferences::fromJson / load (corrupt → moved aside)
//   controller profiles (JSON)  io::controllers::parseProfile, the profile JSON reader (depth, escapes, numbers)
//   MIDI from any device        random bytes → io::midi::MidiParser → ControllerMapper::map of every built-in profile
//   the app's settings file     app::applySettingsText + describeSettings (key=value lines, any bytes)
//   System/resume.json          services::store::Resume::read
//   the peaks cache             services::analysis::PeakCache::read
//   the glyph atlas cache       ui::GlyphAtlas::load (checksum, key, sizes)
//   a save journal (B.commit)   services::store::AtomicWriter::recover (FAT variant's journal: names a staging folder)
//   input scripts               io::parseInputScript (diagnostics, on every platform)
// Every case must return without a crash, hang or unbounded allocation (run under ASan/UBSan by the host preset).
#include "app/SettingsFile.cpp" // the app module is not linked into tests (as tests/unit/app/test_settings_file.cpp)

#include "io/controllers/ScriptedInput.h"
#include "io/controllers/profiles/Json.h"
#include "io/controllers/profiles/Mapper.h"
#include "io/controllers/profiles/Profile.h"
#include "io/midi/MidiParser.h"
#include "services/analysis/Analysis.h"
#include "services/settings/Preferences.h"
#include "services/store/AtomicWriter.h"
#include "services/store/Resume.h"
#include "ui/render/GlyphAtlas.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
struct Rng
{
    std::uint32_t x;
    std::uint32_t next()
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : next() % n; }
};

struct TempDir
{
    juce::File dir;
    explicit TempDir(const char* name)
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile(juce::String("fuzz-") + name + "-" + juce::String(::getpid()) + "-" +
                                juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())))
    {
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
};

// Text inputs (JSON, key=value, scripts): byte edits plus spliced hostile tokens.
std::string mutateText(std::string s, Rng& r)
{
    static const char* tokens[] = {"null", "-1", "1e309", "-1e309", "NaN", "Infinity", "999999999999999999999",
                                   "4294967296", "-2147483649", "\"\\u0000\"", "\"\\ud800\"", "\"\\uDFFF\\uD800\"",
                                   "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[", "{\"a\":{\"a\":{\"a\":{\"a\":{",
                                   "}}}}]]]]", ",", ":", "\"", "\\", "=", "\n", "\r\n", "#", "\xff\xfe", "\xc3",
                                   "0x7fffffff", "-0", "1e-400", "\"../../etc/passwd\"", "pad.17", "enc.0",
                                   "\"status\":256", "\"channel\":-5", "\"number\":300", "\"palette\":99999"};
    const int edits = 1 + static_cast<int>(r.below(5));
    for (int i = 0; i < edits; ++i)
    {
        const std::size_t at = r.below(s.size() + 1);
        switch (r.below(7))
        {
        case 0:
            if (! s.empty())
                s[r.below(s.size())] = static_cast<char>(r.next());
            break;
        case 1: s.resize(at); break;
        case 2: s.insert(at, tokens[r.below(std::size(tokens))]); break;
        case 3:
            if (at < s.size())
                s.erase(at, 1 + r.below(16));
            break;
        case 4: s.insert(at, s.substr(r.below(s.size() + 1), r.below(64))); break;
        case 5:
            for (std::size_t k = 0; k < s.size(); ++k)
                if (std::isdigit(static_cast<unsigned char>(s[k])) && r.below(8) == 0)
                    s[k] = static_cast<char>('0' + r.below(10));
            break;
        default: s.insert(at, std::string(r.below(300), static_cast<char>(r.next()))); break;
        }
    }
    return s;
}

// Binary inputs (caches): flips, cuts, extensions, size fields rewritten.
std::vector<unsigned char> mutateBytes(std::vector<unsigned char> b, Rng& r)
{
    const int edits = 1 + static_cast<int>(r.below(4));
    for (int i = 0; i < edits; ++i)
        switch (r.below(5))
        {
        case 0:
            if (! b.empty())
                b[r.below(b.size())] ^= static_cast<unsigned char>(1u << r.below(8));
            break;
        case 1: b.resize(r.below(b.size() + 1)); break;
        case 2:
            for (std::size_t k = 0, n = r.below(64); k < n; ++k)
                b.push_back(static_cast<unsigned char>(r.next()));
            break;
        case 3: // an int32 / int64 field near the start set to an extreme
            if (b.size() >= 24)
            {
                const std::size_t at = r.below(std::min<std::size_t>(b.size() - 8, 64));
                const std::uint64_t v = r.below(2) ? ~0ull : (r.below(2) ? 0x7fffffffffffffffull : 0x80000000ull);
                std::memcpy(b.data() + at, &v, r.below(2) ? 4 : 8);
            }
            break;
        default:
            if (! b.empty())
                for (std::size_t k = 0, n = r.below(32); k < n; ++k)
                    b[r.below(b.size())] = static_cast<unsigned char>(r.next());
            break;
        }
    return b;
}

void writeBytes(const juce::File& f, const std::vector<unsigned char>& b) { REQUIRE(f.replaceWithData(b.data(), b.size())); }
void writeText(const juce::File& f, const std::string& s) { REQUIRE(f.replaceWithData(s.data(), s.size())); }
} // namespace

TEST_CASE("fuzz: 600 mutated preferences files load as defaults or values, never crash", "[reliability][fuzz]")
{
    services::settings::Preferences base;
    base.set("project.resume", 0);
    base.set("prefs.pads.velocityCurve", 5);
    base.set("ui.scale", 150);
    base.setString("audio.deviceName", "hw:0");
    base.setList("library.locations", {"/media/a", "/media/b"});
    const std::string valid = base.toJson();
    TempDir tmp("prefs");
    Rng r {0xC0FFEEu};
    int accepted = 0;
    for (int i = 0; i < 600; ++i)
    {
        const auto text = mutateText(valid, r);
        services::settings::Preferences p;
        if (p.fromJson(juce::String::fromUTF8(text.data(), static_cast<int>(text.size()))))
        {
            ++accepted;
            (void) p.get("ui.scale");
            (void) p.getString("audio.deviceName");
            (void) p.getList("library.locations");
            (void) p.toJson();
        }
        if (i % 10 == 0) // the file path: a corrupt file is moved aside, the defaults are used
        {
            const auto f = tmp.dir.getChildFile("preferences.json");
            writeText(f, text);
            services::settings::Preferences q;
            (void) q.load(f);
            (void) q.get("project.resume");
            tmp.dir.getChildFile("preferences.json.corrupt").deleteFile();
        }
    }
    std::printf("fuzz prefs: %d of 600 mutated files still parsed\n", accepted);
}

TEST_CASE("fuzz: 1,500 mutated controller profiles and raw JSON never crash the reader or the validator",
          "[reliability][fuzz]")
{
    Rng r {0xBADC0DEu};
    int accepted = 0;
    const auto texts = io::controllers::builtinProfileTexts();
    REQUIRE(! texts.empty());
    for (int i = 0; i < 1500; ++i)
    {
        const std::string base(texts[static_cast<std::size_t>(i) % texts.size()].json);
        const auto text = mutateText(base, r);
        if (auto p = io::controllers::parseProfile(text))
        {
            ++accepted;
            const auto again = io::controllers::parseProfile(io::controllers::toJson(p.value())); // the save path
            CHECK(again.ok());
            io::controllers::ControllerMapper m(p.value(), 3);
            io::controllers::MapResult out;
            for (int k = 0; k < 64; ++k)
            {
                const std::uint8_t msg[3] = {static_cast<std::uint8_t>(0x80 | r.below(0x80)),
                                             static_cast<std::uint8_t>(r.below(128)),
                                             static_cast<std::uint8_t>(r.below(128))};
                out = {};
                (void) m.map(msg, 1 + static_cast<int>(r.below(3)), k * 1.0, out, nullptr);
            }
        }
        (void) io::controllers::json::parse(text);
    }
    // Pathological nesting and sizes for the reader itself.
    CHECK_FALSE(io::controllers::json::parse(std::string(100000, '[')).ok());
    CHECK_FALSE(io::controllers::json::parse(std::string(100000, '{')).ok());
    (void) io::controllers::json::parse("\"" + std::string(1 << 20, 'x') + "\"");
    (void) io::controllers::json::parse("[" + std::string(400, '-') + "1]");
    std::printf("fuzz profiles: %d of 1500 mutated profiles still valid\n", accepted);
}

TEST_CASE("fuzz: 200,000 random MIDI bytes through the parser and every built-in profile's mapper",
          "[reliability][fuzz]")
{
    const auto& profiles = io::controllers::builtinProfiles();
    REQUIRE(! profiles.empty());
    std::vector<std::unique_ptr<io::controllers::ControllerMapper>> mappers; // not movable (atomics)
    for (const auto& p : profiles)
        mappers.push_back(std::make_unique<io::controllers::ControllerMapper>(p, 2));
    io::midi::MidiParser parser;
    Rng r {0x5EEDu};
    int messages = 0;
    for (int i = 0; i < 200000; ++i)
    {
        // biased towards status bytes, SysEx starts / ends and realtime bytes
        std::uint8_t b = static_cast<std::uint8_t>(r.next());
        if (r.below(6) == 0)
            b = static_cast<std::uint8_t>(0xF0 | r.below(16));
        core::midi::MidiEvent e;
        if (parser.feed(b, e))
        {
            ++messages;
            REQUIRE(e.size >= 1);
            REQUIRE(e.size <= 3);
            for (auto& m : mappers)
            {
                io::controllers::MapResult out;
                (void) m->map(e.bytes, e.size, i * 0.01, out, nullptr);
                REQUIRE(out.count >= 0);
                REQUIRE(out.count <= static_cast<int>(out.events.size()));
            }
        }
    }
    for (auto& m : mappers) // MMC-like SysEx of every length straight into map()
        for (int len = 0; len <= 8; ++len)
        {
            std::uint8_t sx[8] = {0xF0, 0x7F, 0x7F, 0x06, static_cast<std::uint8_t>(r.below(256)), 0xF7, 0xF7, 0xF7};
            io::controllers::MapResult out;
            (void) m->map(sx, len, 0, out, nullptr);
        }
    std::printf("fuzz midi: %d messages parsed from 200000 bytes\n", messages);
    CHECK(messages > 1000);
}

TEST_CASE("fuzz: 1,000 mutated settings files", "[reliability][fuzz]")
{
    const std::string valid = "# device settings\nrender=gl-fb,soft\naudio=1\naudio_device=hw:ak4458audio,0\n"
                              "run_frames=0\ndev_kit=Samples/Kit\nlive__inputLatency = 96\n  Input_Trace = 1\n"
                              "diag_interval=10\nmidi_providers=alsa,virtual\n";
    const app::Settings defaults {{"render", "auto"}, {"audio", "1"}, {"dev_kit", ""}};
    Rng r {0x5E77u};
    for (int i = 0; i < 1000; ++i)
    {
        const auto text = mutateText(valid, r);
        app::Settings s = defaults;
        const int n = app::applySettingsText(s, text);
        CHECK(n >= 0);
        app::SettingsOrigin origin;
        origin.file = "/media/usb/settings.conf";
        (void) app::describeSettings(s, defaults, origin);
        for (const auto& [k, v] : s)
            (void) app::settingsEnvironmentName("FUZZ", k);
    }
}

TEST_CASE("fuzz: 800 mutated resume.json files", "[reliability][fuzz]")
{
    TempDir tmp("resume");
    const auto system = tmp.dir.getChildFile("System");
    system.createDirectory();
    services::store::Resume resume({system, system.getChildFile("autosave")});
    const std::string valid =
        "{\"format\": {\"name\": \"" + std::string("sixteen-resume") + "\", \"version\": 1}, \"project\": \"" +
        tmp.dir.getChildFile("Projects/Song.sixteen").getFullPathName().toStdString() +
        "\", \"selection\": {\"scene\": 3, \"pattern\": 2, \"track\": 7, \"screen\": \"track.lfo\"}, "
        "\"earlyExits\": 1}";
    Rng r {0x2E5u};
    for (int i = 0; i < 800; ++i)
    {
        writeText(resume.resumeFile(), mutateText(valid, r));
        if (const auto rec = resume.read())
        {
            CHECK(rec->selection.scene >= 0);
            CHECK(rec->selection.scene <= 15);
            CHECK(rec->earlyExits >= 0);
            CHECK(rec->selection.screen.size() <= 64 * 4);
        }
    }
    writeText(resume.resumeFile(), std::string(70 * 1024, '{')); // over the size cap
    CHECK_FALSE(resume.read().has_value());
}

TEST_CASE("fuzz: 2,000 mutated peaks cache files are refused or exact", "[reliability][fuzz]")
{
    TempDir tmp("peaks");
    services::analysis::PeakOverview o;
    o.bucket = 64;
    o.frames = 64 * 40 - 5;
    for (int i = 0; i < 40; ++i)
    {
        o.min.push_back(-0.01f * static_cast<float>(i));
        o.max.push_back(0.01f * static_cast<float>(i));
    }
    const auto file = tmp.dir.getChildFile("feed-64.pks");
    REQUIRE(services::analysis::PeakCache::write(file, o));
    juce::MemoryBlock m;
    REQUIRE(file.loadFileAsData(m));
    const std::vector<unsigned char> valid(static_cast<const unsigned char*>(m.getData()),
                                           static_cast<const unsigned char*>(m.getData()) + m.getSize());
    Rng r {0x9EAu};
    int accepted = 0;
    for (int i = 0; i < 2000; ++i)
    {
        writeBytes(file, mutateBytes(valid, r));
        if (const auto got = services::analysis::PeakCache::read(file))
        {
            ++accepted;
            REQUIRE(got->bucket >= 1);
            REQUIRE(got->min.size() == got->max.size());
            REQUIRE(static_cast<std::int64_t>(got->min.size()) == (got->frames + got->bucket - 1) / got->bucket);
            for (std::size_t k = 0; k < got->min.size(); ++k)
                REQUIRE((std::isfinite(got->min[k]) && std::isfinite(got->max[k])));
        }
    }
    // the overflow case found here: frames near INT64_MAX made (frames + bucket - 1) overflow before the size check
    std::vector<unsigned char> big = valid;
    const std::int64_t huge = 0x7fffffffffffffffll;
    for (int k = 0; k < 8; ++k)
        big[8 + static_cast<std::size_t>(k)] = static_cast<unsigned char>(static_cast<std::uint64_t>(huge) >> (56 - 8 * k));
    writeBytes(file, big);
    CHECK_FALSE(services::analysis::PeakCache::read(file).has_value());
    std::printf("fuzz peaks: %d of 2000 mutated files accepted (a flipped value bit keeps a valid file)\n", accepted);
}

TEST_CASE("fuzz: 300 mutated glyph atlas caches load exactly or are refused", "[reliability][fuzz]")
{
    TempDir tmp("atlas");
    ui::GlyphAtlas atlas;
    std::string error;
    REQUIRE(atlas.build(0.625f, error));
    const auto path = tmp.dir.getChildFile("atlas.bin").getFullPathName().toStdString();
    REQUIRE(atlas.save(path, error));
    juce::MemoryBlock m;
    REQUIRE(juce::File(path).loadFileAsData(m));
    const std::vector<unsigned char> valid(static_cast<const unsigned char*>(m.getData()),
                                           static_cast<const unsigned char*>(m.getData()) + m.getSize());
    Rng r {0xA71A5u};
    int loaded = 0;
    for (int i = 0; i < 300; ++i)
    {
        auto bytes = mutateBytes(valid, r);
        writeBytes(juce::File(path), bytes);
        ui::GlyphAtlas a;
        std::string why;
        if (a.load(path, 0.625f, why))
        {
            ++loaded;
            REQUIRE(bytes == valid); // the checksum lets nothing else through
        }
    }
    std::printf("fuzz atlas: %d of 300 mutated caches loaded (each one byte-identical)\n", loaded);
}

TEST_CASE("fuzz: 400 garbage save journals never move files from elsewhere or lose the bundle", "[reliability][fuzz]")
{
    TempDir tmp("journal");
    const auto parent = tmp.dir.getChildFile("Projects");
    const auto outside = tmp.dir.getChildFile("Elsewhere");
    Rng r {0x10C4u};
    const char* words[] = {"seal", "move-old", "install", "clean", "", "seal ", "x"};
    for (int i = 0; i < 400; ++i)
    {
        parent.deleteRecursively();
        outside.deleteRecursively();
        parent.createDirectory();
        outside.createDirectory();
        const auto bundle = parent.getChildFile("Song.sixteen");
        bundle.createDirectory();
        writeText(bundle.getChildFile("project.json"), "{\"v\": \"current\"}");
        writeText(outside.getChildFile("precious.txt"), "not part of any project");
        if (r.below(2)) // a sealed copy, a backup, a staging folder, in any combination
        {
            services::store::AtomicWriter::sealedOf(bundle).createDirectory();
            writeText(services::store::AtomicWriter::sealedOf(bundle).getChildFile("project.json"), "{\"v\": \"new\"}");
        }
        if (r.below(2))
        {
            services::store::AtomicWriter::backupOf(bundle).createDirectory();
            writeText(services::store::AtomicWriter::backupOf(bundle).getChildFile("project.json"), "{\"v\": \"old\"}");
        }
        const juce::String staging = "Song.sixteen.tmp-123-0badf00d";
        if (r.below(2))
            parent.getChildFile(staging).createDirectory();
        std::string journal = words[r.below(std::size(words))];
        switch (r.below(6))
        {
        case 0: journal += " ../Elsewhere/precious.txt"; break;
        case 1: journal += " ../Elsewhere"; break;
        case 2: journal += " Song.sixteen"; break;
        case 3: journal += " " + staging.toStdString(); break;
        case 4: journal += " /" + outside.getChildFile("precious.txt").getFullPathName().toStdString(); break;
        default: journal = mutateText(journal + " " + staging.toStdString(), r); break;
        }
        writeText(services::store::AtomicWriter::journalOf(bundle), journal);
        INFO("journal: " << journal);
        (void) services::store::AtomicWriter::recover(bundle);
        services::store::AtomicWriter::recoverLeftovers(parent);
        REQUIRE(outside.getChildFile("precious.txt").loadFileAsString() == "not part of any project");
        // a complete bundle is installed (the current, the sealed or the previous one) or kept as an orphan
        const bool installed = bundle.getChildFile("project.json").existsAsFile();
        const bool orphaned = ! parent.findChildFiles(juce::File::findDirectories, false, "Song.sixteen.orphan-*").isEmpty();
        REQUIRE((installed || orphaned));
        for (const auto& f : parent.findChildFiles(juce::File::findFilesAndDirectories, true, "precious.txt"))
            FAIL("moved into the project folder: " << f.getFullPathName());
    }
}

TEST_CASE("fuzz: 1,000 mutated input scripts parse or report errors", "[reliability][fuzz]")
{
    const std::string valid = "# script\n0 press pad1 100\n50 release pad1\n100 turn knob1 -3\n150 value enc.2 64\n"
                              "200 strip 64\n250 strip off\n300 touch 400 240\n320 drag 410 250\n340 release-touch\n"
                              "400 tap 10 10\n500 cmd render-job pattern bars=1\n600 screenshot /tmp/x.png\n700 quit\n";
    Rng r {0x5C21u};
    for (int i = 0; i < 1000; ++i)
    {
        const auto res = io::parseInputScript(mutateText(valid, r));
        for (std::size_t k = 1; k < res.steps.size(); ++k)
            REQUIRE(res.steps[k - 1].atMs <= res.steps[k].atMs); // sorted, whatever the times
    }
}
