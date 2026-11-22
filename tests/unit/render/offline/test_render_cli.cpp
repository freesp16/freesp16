// The render CLI (<name>_render_cli) over a whole generated project bundle — saved by the native codec,
// samples generated into its samples/ folder ($Bundle). Checks: the CLI's file, sidecar and hash equal the in-process
// render of the same bundle (the render service's core, the same sample loading), two CLI runs agree, and each case's
// hash equals the stored one for this build configuration (render-hashes.txt: "<case> <config> <hash>"; config =
// <arch>-<release|debug>, since -O0 and -O3 code may round differently). A configuration without stored hashes
// WARNs and writes the hashes it got to <artifacts>/render-hashes.<config>.txt to be committed.
#include "ProjectIdentity.h"
#include "core/params/ParamRegistry.h"
#include "services/library/SidecarStore.h"
#include "services/render/ProjectSamples.h"
#include "services/render/RenderService.h"
#include "services/store/CodecRegistry.h"
#include "tests/unit/library/LibTestAudio.h"
#include "tests/unit/render/offline/RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace rnd = services::render;

namespace
{
std::string config()
{
#if defined(__x86_64__)
    std::string arch = "x86_64";
#elif defined(__aarch64__)
    std::string arch = "arm64";
#elif defined(__arm__)
    std::string arch = "armel";
#else
    std::string arch = "other";
#endif
#ifdef NDEBUG
    return arch + "-release";
#else
    return arch + "-debug";
#endif
}

juce::File cli()
{
    return juce::File(APP_ARTIFACT_DIR).getParentDirectory().getChildFile("bin").getChildFile(
        juce::String(identity::name) + "_render_cli");
}

// The generated bundle: the RenderTestSupport project with its samples written as 16-bit WAVs into samples/.
juce::File writeBundle(const juce::File& dir)
{
    auto f = rendertest::makeFixture(true);
    auto& p = *f.project;
    const auto bundle = dir.getChildFile("Demo.sixteen");
    const char* names[] = {"kick.wav", "hat.wav", "ring.wav"};
    std::vector<juce::File> files;
    for (int t = 0; t < 3; ++t)
    {
        const auto& b = *f.buffers[static_cast<std::size_t>(t)];
        libtest::Spec s;
        std::vector<std::vector<float>> planar(1, std::vector<float>(b.ch[0], b.ch[0] + b.frames));
        files.push_back(dir.getChildFile(names[t]));
        REQUIRE(libtest::writeFile(files.back(), libtest::encode(s, planar)));
        const auto id = services::store::contentId(files.back());
        REQUIRE(id);
        p.sampleRefs.entries.push_back({id.value(), std::string("$Bundle/") + names[t], names[t], false, {}});
        p.scenes[0].tracks[static_cast<std::size_t>(t)].playback.sample.id = id.value();
    }
    // Pattern 2 of scene 2 for the scene / arrangement cases; a 3-row song.
    p.scenes[1] = p.scenes[0];
    p.arrangement.sections = {{0, 0, 1, std::nullopt, {}}, {0, 1, 2, 140.0, {}}, {1, 0, 1, std::nullopt, {}}};
    services::store::SaveOptions so;
    so.timestamp = "2026-01-01T00:00:00Z";
    REQUIRE(services::store::saveProject(p, bundle, so));
    REQUIRE(bundle.getChildFile("samples").createDirectory());
    for (std::size_t i = 0; i < files.size(); ++i)
        REQUIRE(files[i].copyFileTo(bundle.getChildFile("samples").getChildFile(names[i])));
    return bundle;
}

std::map<std::string, std::string> fields(const juce::String& line)
{
    std::map<std::string, std::string> kv;
    for (const auto& tok : juce::StringArray::fromTokens(line.fromFirstOccurrenceOf("RENDER ", false, false), " ", ""))
        kv[tok.upToFirstOccurrenceOf("=", false, false).toStdString()] = tok.fromFirstOccurrenceOf("=", false, false).toStdString();
    return kv;
}

juce::String run(const juce::StringArray& args, int& code)
{
    juce::ChildProcess proc;
    REQUIRE(proc.start(args, juce::ChildProcess::wantStdOut));
    const auto out = proc.readAllProcessOutput();
    REQUIRE(proc.waitForProcessToFinish(120000));
    code = static_cast<int>(proc.getExitCode());
    return out;
}

std::map<std::string, std::string> storedHashes()
{
    std::map<std::string, std::string> out;
    std::ifstream in(std::string(APP_SOURCE_DIR) + "/tests/unit/render/offline/render-hashes.txt");
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        std::string c, cfg, h;
        if (s >> c >> cfg >> h && cfg == config())
            out[c] = h;
    }
    return out;
}

struct Case
{
    const char* name;
    std::vector<const char*> args;
    rnd::RenderJob job;
    bool reference = false;
};
} // namespace

TEST_CASE("offline render: the render CLI renders whole projects, equal to the service and to the stored hashes")
{
    const auto exe = cli();
    if (! exe.existsAsFile())
        SKIP("no render CLI at " << exe.getFullPathName() << " (APP_BUILD_HOST_APP off)");
    libtest::TempDir tmp("render-cli");
    const auto bundle = writeBundle(tmp.dir());

    std::vector<Case> cases;
    auto add = [&](const char* name, std::vector<const char*> args, auto edit, bool reference = false)
    {
        Case c {name, std::move(args), {}, reference};
        edit(c.job);
        cases.push_back(std::move(c));
    };
    add("pattern-2bars", {"--bars", "2"}, [](rnd::RenderJob& j) { j.bars = 2; });
    add("pattern-leave", {"--remainder", "leave", "--bits", "16"}, [](rnd::RenderJob& j) { j.tails = true; j.bitDepth = 16; });
    add("scene", {"--source", "scene", "--scene", "2"}, [](rnd::RenderJob& j) { j.source = rnd::Source::Scene; j.scene = 1; });
    add("arrangement", {"--source", "arrangement"}, [](rnd::RenderJob& j) { j.source = rnd::Source::Arrangement; });
    add("track-direct", {"--source", "track", "--track", "2", "--fx", "0"},
        [](rnd::RenderJob& j) { j.source = rnd::Source::Track; j.track = 1; j.includeBusFx = false; });
    add("reference-quirks", {"--quirks", "reference", "--bits", "32"}, [](rnd::RenderJob& j) { j.bitDepth = 32; }, true);

    // In process: the same bundle, the same sample loading, the CLI's feeder.
    auto opened = services::store::openProject(bundle);
    REQUIRE(opened);
    auto& project = *opened.value();
    services::store::SampleRoots roots;
    roots.bundle = bundle;
    auto loaded = rnd::LoadedSamples::load(project, roots, 44100);
    REQUIRE(loaded);
    CHECK(loaded.value()->errors().empty());
    CHECK(loaded.value()->snapshot().count() == 6); // 3 tracks × scenes 1 and 2

    const auto stored = storedHashes();
    std::string got;
    for (const auto& c : cases)
    {
        INFO("case " << c.name);
        const auto out = tmp.dir().getChildFile(juce::String(c.name) + ".wav");
        juce::StringArray args {exe.getFullPathName(), "--project", bundle.getFullPathName(), "--out", out.getFullPathName()};
        for (const auto* a : c.args)
            args.add(a);
        int code = -1;
        const auto line = run(args, code);
        REQUIRE(code == 0);
        auto kv = fields(line.trim());
        REQUIRE(kv.count("hash"));
        CHECK(out.existsAsFile());
        CHECK(services::library::SidecarStore::jsonFor(out).existsAsFile());
        int code2 = -1;
        args.add("--no-write");
        CHECK(fields(run(args, code2).trim())["hash"] == kv["hash"]); // a second process: the same output
        CHECK(code2 == 0);

        const bool reference = c.reference;
        const auto feed = app::feed::feeder(true);
        const rnd::ProjectFeeder feeder = [&feed, reference](const core::model::Project& p, engine::EngineBridge& to,
                                                             core::arrangement::SongStatus* s,
                                                             const rnd::SampleSnapshot& samples)
        {
            feed(p, to, s, samples);
            to.post(engine::ParamChange {core::builtinParams().idOf("fx.quirkProfile").value(), 0, reference ? 1.0f : 0.0f, 0});
        };
        const auto r = rnd::render(project, c.job, loaded.value()->snapshot(), feeder, 44100);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(r.hash));
        CHECK(kv["hash"] == hex); // the CLI is the service's render
        CHECK(std::stoll(kv["frames"]) == static_cast<long long>(r.left.size()));
        CHECK(rendertest::peak(r.left) > 0.01);
        got += std::string(c.name) + " " + config() + " " + kv["hash"] + "\n";
        if (stored.count(c.name))
            CHECK(stored.at(c.name) == kv["hash"]);
    }
    if (stored.size() < cases.size())
    {
        const auto file = juce::File(APP_ARTIFACT_DIR).getChildFile("render-hashes." + config() + ".txt");
        file.getParentDirectory().createDirectory();
        file.replaceWithText(got);
        WARN("no stored render hashes for " << config() << ": wrote " << file.getFullPathName());
    }
}

TEST_CASE("offline render: the render CLI refuses bad arguments and missing projects")
{
    const auto exe = cli();
    if (! exe.existsAsFile())
        SKIP("no render CLI");
    int code = -1;
    run({exe.getFullPathName()}, code);
    CHECK(code == 2);
    run({exe.getFullPathName(), "--project", "/nonexistent/x.sixteen", "--out", "/tmp/never.wav"}, code);
    CHECK(code == 1);
    run({exe.getFullPathName(), "--project", "x", "--out", "y", "--source", "stems"}, code);
    CHECK(code == 2);
}
