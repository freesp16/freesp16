// <name>_render_cli — offline render of a project file to a WAV (REN-001…003).
//
//   <name>_render_cli --project P [--scene S] [--pattern N] [--bars B] --out F.wav [--quirks reference]
//                     [--source pattern|track|scene|arrangement] [--track T] [--repeats N] [--fx 0|1]
//                     [--bits 16|24|32] [--remainder cut|wrap|leave] [--normalize dBFS] [--rate HZ]
//                     [--library DIR] [--plugins DIR] [--no-write]
//
//   P          a project the registered codecs read (a native .sixteen bundle); its samples resolve against the
//              bundle ($Bundle) and --library ($Lib); absolute paths as they are
//   S, N, T    1-based scene / pattern / track (default 1 / 1 / 1)
//   B          the length in bars (default: the source's own length × repeats)
//   --quirks reference   the reference FX profile (fx.quirkProfile = REFERENCE, FX-053) instead of the app's
//   --no-write           render and print, write no file (hash checks)
// Prints one line `RENDER file=… frames=… rate=… bars=… bpm=… peak=… hash=<FNV-1a 64 of the float output>` and exits 0;
// exit 1 on an error (the message on stderr), 2 on a usage error. The render is the render service's synchronous core
// (services::render::render) with the app's project forwarding: bit-identical to the app's offline render of the same
// project and job on the same build.
#include "app/ProjectFeed.h"
#include "core/params/ParamRegistry.h"
#include "services/render/ProjectSamples.h"
#include "services/render/RenderService.h"
#include "services/store/CodecRegistry.h"
#include "services/store/SampleRefs.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace
{
namespace rnd = services::render;
namespace store = services::store;

int usage(const char* why)
{
    std::fprintf(
        stderr,
        "%s\nusage: render_cli --project P [--scene S] [--pattern N] [--bars B] --out F.wav [--quirks reference]\n"
        "       [--source pattern|track|scene|arrangement] [--track T] [--repeats N] [--fx 0|1] [--bits 16|24|32]\n"
        "       [--remainder cut|wrap|leave] [--normalize dBFS] [--rate HZ] [--library DIR] [--plugins DIR] "
        "[--no-write]\n",
        why);
    return 2;
}

int fail(const std::string& what, const core::Error& e)
{
    std::fprintf(stderr, "render_cli: %s: %s%s%s\n", what.c_str(), e.message.c_str(), e.detail.empty() ? "" : " — ",
                 e.detail.c_str());
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
    std::map<std::string, std::string> opt;
    bool noWrite = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--no-write")
            noWrite = true;
        else if (a.rfind("--", 0) == 0 && i + 1 < argc)
            opt[a.substr(2)] = argv[++i];
        else
            return usage(("unexpected argument " + a).c_str());
    }
    if (! opt.count("project") || (! opt.count("out") && ! noWrite))
        return usage("--project and --out are required");
    const auto num = [&](const char* k, double d) { return opt.count(k) ? std::atof(opt[k].c_str()) : d; };

    rnd::RenderJob job;
    const std::string source = opt.count("source") ? opt["source"] : "pattern";
    if (source == "pattern")
        job.source = rnd::Source::Pattern;
    else if (source == "track")
        job.source = rnd::Source::Track;
    else if (source == "scene")
        job.source = rnd::Source::Scene;
    else if (source == "arrangement")
        job.source = rnd::Source::Arrangement;
    else
        return usage("--source: pattern, track, scene or arrangement");
    job.scene = static_cast<int>(num("scene", 1)) - 1;
    job.pattern = static_cast<int>(num("pattern", 1)) - 1;
    job.track = static_cast<int>(num("track", 1)) - 1;
    job.repeats = std::max(1, static_cast<int>(num("repeats", 1)));
    job.bars = std::max(0.0, num("bars", 0));
    job.includeBusFx = num("fx", 1) >= 0.5;
    job.bitDepth = static_cast<int>(num("bits", 24));
    const std::string remainder = opt.count("remainder") ? opt["remainder"] : "cut";
    job.tails = remainder == "leave";
    job.wrapTail = remainder == "wrap";
    if (opt.count("normalize"))
    {
        job.normalize = true;
        job.normalizeDbfs = num("normalize", -1.0);
    }
    const double rate = num("rate", 44100);
    const bool reference = opt.count("quirks") && opt["quirks"] == "reference";
    if (job.scene < 0 || job.scene > 15 || job.pattern < 0 || job.pattern > 15 || job.track < 0 || job.track > 15)
        return usage("--scene / --pattern / --track: 1…16");

    // The project and its samples.
    const juce::File projectFile = juce::File::getCurrentWorkingDirectory().getChildFile(opt["project"]);
    auto opened = store::openProject(projectFile);
    if (! opened)
        return fail("open " + projectFile.getFullPathName().toStdString(), opened.error());
    auto project = std::move(opened.value());
    store::SampleRoots roots;
    roots.bundle = projectFile;
    if (opt.count("library"))
        roots.library = juce::File::getCurrentWorkingDirectory().getChildFile(opt["library"]);
    auto loaded = rnd::LoadedSamples::load(*project, roots, rate);
    if (! loaded)
        return fail("samples", loaded.error());
    for (const auto& e : loaded.value()->errors())
        std::fprintf(stderr, "render_cli: sample %s\n", e.c_str());

    // The render: the app's forwarding, then the FX profile.
    const auto feed = app::feed::feeder(true);
    const rnd::ProjectFeeder feeder = [&feed, reference](const core::model::Project& p, engine::EngineBridge& to,
                                                         core::arrangement::SongStatus* status,
                                                         const rnd::SampleSnapshot& samples)
    {
        feed(p, to, status, samples);
        if (const auto id = core::builtinParams().idOf("fx.quirkProfile"))
            to.post(engine::ParamChange {*id, 0, reference ? 1.0f : 0.0f, 0});
    };
    const auto plugins = opt.count("plugins")
                             ? juce::File::getCurrentWorkingDirectory().getChildFile(opt["plugins"])
                             : projectFile.getParentDirectory().getParentDirectory().getChildFile("Plugins");
    const auto r = rnd::render(*project, job, loaded.value()->snapshot(), feeder, rate, nullptr, nullptr,
                               plugins.getFullPathName().toStdString());
    double peak = 0;
    for (std::size_t i = 0; i < r.left.size(); ++i)
        peak = std::max({peak, static_cast<double>(std::fabs(r.left[i])), static_cast<double>(std::fabs(r.right[i]))});
    std::string written = "-";
    if (! noWrite)
    {
        const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(opt["out"]);
        auto w = rnd::writeRendered(r, job, out);
        if (! w)
            return fail("write " + out.getFullPathName().toStdString(), w.error());
        written = out.getFullPathName().toStdString();
    }
    std::printf("RENDER file=%s frames=%zu rate=%.0f bars=%.3f bpm=%.3f peak=%.6f preroll=%d hash=%016llx\n",
                written.c_str(), r.left.size(), rate, r.sixteenths / 16.0, r.bpm, peak, r.preroll,
                static_cast<unsigned long long>(r.hash));
    return 0;
}
