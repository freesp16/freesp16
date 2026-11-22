// Project benchmarks. Budgets are Cortex-A9 targets; the
// host measures the same work (host-release preset), so its figure is a lower bound, not the A9 number (open until
// measured on a unit; scripts/emulator-bench.sh bench_project gives relative numbers only):
//   bench_project_snapshot  message-thread cost of a save / an autosave: one copy of a full project    A9 ≤ 4 ms
//   bench_project_save      full 16-scene project: backup of the old version + atomic write + rotation  A9 ≤ 1.5 s
//   bench_project_load      full project, no sample decode: guards + read + migrate + decode + resolve  A9 ≤ 1 s
//   bench_project_list      200 projects in OPEN (meta.json head + pattern-file sizes only)             A9 ≤ 200 ms
// "Full" = every scene, track and pattern used, 25 % of the step cells filled (the store's bench project). Saves go
// through the real AtomicWriter (fsync + renames) in a temp folder.
#include "tests/unit/store/StoreTestSupport.h"

#include "core/commands/CommandBusAccess.h"
#include "services/library/Workers.h"
#include "services/store/CodecRegistry.h"
#include "services/store/ProjectStore.h"
#include "tests/bench/harness/Bench.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace services::store;

namespace
{
void need(bool ok, const char* what)
{
    if (! ok)
    {
        std::printf("BENCH bench_project: %s failed\n", what);
        std::abort();
    }
}

// A fresh folder in the system temp dir, removed afterwards.
struct Scratch
{
    explicit Scratch(const char* name)
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile(juce::String("bench-project-") + name + "-" +
                                juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())))
    {
        dir.createDirectory();
    }
    ~Scratch() { dir.deleteRecursively(); }
    juce::File dir;
};

const core::model::Project& fullProject()
{
    static const auto p = storetest::randomProject(2026, 0.25, true);
    return *p;
}
} // namespace

BENCH(bench_project_snapshot, 4000000) // 4 ms
{
    const auto& full = fullProject();
    auto snapshot = std::make_shared<core::model::Project>(full);
    state.run([&] { *snapshot = full; bench::doNotOptimize(snapshot->settings.tempo); });
}

BENCH(bench_project_save, 1500000000) // 1.5 s
{
    Scratch s("save");
    const auto bundle = s.dir.getChildFile("Full.sixteen");
    WriteOptions options;
    options.backupFolder = s.dir.getChildFile("Backups/Full");
    for (int i = 0; i < 11; ++i) // the old version exists and the backup folder is full: every save rotates
        need(writeProjectVersion(fullProject(), bundle, options).ok(), "save");
    state.run([&] { need(writeProjectVersion(fullProject(), bundle, options).ok(), "save"); });
}

BENCH(bench_project_load, 1000000000) // 1 s
{
    Scratch s("load");
    const auto bundle = s.dir.getChildFile("Full.sixteen");
    need(saveProject(fullProject(), bundle).ok(), "save");
    auto loaded = std::make_unique<core::model::Project>();
    SampleRoots roots;
    roots.library = s.dir.getChildFile("Lib");
    roots.bundle = bundle;
    state.run(
        [&]
        {
            need(checkBundleInputs(bundle).ok(), "guards");
            ImportReport report;
            need(openProjectInto(bundle, *loaded, report).ok(), "load");
            bench::doNotOptimize(resolvePool(loaded->sampleRefs, roots));
        });
}

BENCH(bench_project_list, 200000000) // 200 ms
{
    Scratch s("list");
    const auto projects = s.dir.getChildFile("Projects");
    projects.createDirectory();
    const auto small = storetest::randomProject(7, 0.05);
    need(saveProject(*small, projects.getChildFile("P000.sixteen")).ok(), "save");
    for (int i = 1; i < 200; ++i)
        need(projects.getChildFile("P000.sixteen")
                 .copyDirectoryTo(projects.getChildFile(juce::String::formatted("P%03d.sixteen", i))),
             "copy");
    services::library::Workers workers {1};
    core::CommandBus bus;
    ProjectStore store(bus, workers, {.projectsDir = projects});
    need(store.list(SortBy::Name).size() == 200, "list");
    state.run([&] { bench::doNotOptimize(store.list(SortBy::Name).size()); });
}
