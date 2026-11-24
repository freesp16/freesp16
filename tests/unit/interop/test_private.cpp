// Your own files, local only (label interop-private through tests/unit/interop-private/test_private_projects.py):
// every .prj / .rsm in tests/interop/private/ (git-ignored, never in CI) must round-trip byte-identical at tree level
// (FMT-043), import without an error and leave the files untouched (FMT-045); the import report is printed for
// sending back. Every .wav.dat there must parse. A native bundle <name>.sixteen next to <name>.prj (saved from that
// project by an older app) must open as what a fresh import gives (MigrationV3.cpp): cells, locks, envelope times
// and the records the exporter writes. Hidden ([.private]): skipped when the folder is empty.
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/store/CodecRegistry.h"
#include "services/interop/sp16/ValueTreeCodec.h"
#include "services/store/SampleRefs.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace services::interop::sp16;

TEST_CASE("prj private: your own project files", "[.private]")
{
    juce::File dir(std::getenv("INTEROP_PRIVATE_DIR") != nullptr
                       ? juce::String(std::getenv("INTEROP_PRIVATE_DIR"))
                       : interop_test::sourceDir().getChildFile("tests/interop/private").getFullPathName());
    std::vector<juce::File> files;
    if (dir.isDirectory())
        for (const auto& e : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles))
            files.push_back(e.getFile());
    std::sort(files.begin(), files.end());
    int checked = 0;
    for (const auto& f : files)
    {
        const auto name = f.getFileName();
        INFO(f.getFullPathName().toStdString());
        if (name.endsWithIgnoreCase(".wav.dat"))
        {
            auto w = readWavDat(f);
            CHECK(w);
            if (w)
                std::printf("WAVDAT %s bar=%g bpm=%g sampleNum=%d\n", name.toRawUTF8(), w.value().bar, w.value().bpm,
                            w.value().sampleNum);
            ++checked;
            continue;
        }
        if (! name.endsWithIgnoreCase(".prj") && ! name.endsWithIgnoreCase(".rsm"))
            continue;
        const auto before = services::store::contentId(f);
        const auto mtime = f.getLastModificationTime();
        const auto bytes = interop_test::bytesOf(f);
        auto tree = readContainer(bytes);
        REQUIRE(tree);
        const bool same = writeContainer(tree.value()) == bytes;
        CHECK(same);
        services::store::ImportReport report;
        auto p = importProjectFile(f, {}, report);
        CHECK(p);
        std::printf("== %s\n", f.getFullPathName().toRawUTF8());
        if (! p)
            std::printf("IMPORT FAILED %s\n", p.error().message.c_str());
        for (const auto* list : {&report.warnings, &report.unmapped})
            for (const auto& l : *list)
                std::printf("IMPORT %s\n", l.c_str());
        // Every programmed record names its own track and step (track-major matrix, StepMatrix.h), and the model
        // writes every pattern's records back unchanged.
        bool mismatch = false, verbatim = false;
        for (const auto* list : {&report.warnings, &report.unmapped})
            for (const auto& l : *list)
            {
                mismatch = mismatch || l.find("names another track / step") != std::string::npos;
                verbatim = verbatim || l.find("PatternData.Steps kept verbatim") != std::string::npos;
            }
        CHECK_FALSE(mismatch);
        CHECK_FALSE(verbatim);
        if (const auto bundle = f.getSiblingFile(f.getFileNameWithoutExtension() + ".sixteen"); p && bundle.isDirectory())
        {
            INFO(bundle.getFullPathName().toStdString());
            core::model::Project old;
            services::store::ImportReport r2;
            REQUIRE(services::store::openProjectInto(bundle, old, r2).ok());
            for (const auto& w : r2.warnings)
                std::printf("MIGRATE %s\n", w.c_str());
            const auto& fresh = *p.value();
            int cells = 0;
            for (std::size_t s = 0; s < fresh.scenes.size(); ++s)
            {
                const auto fx = insertTypesOf(fresh.scenes[s]);
                for (std::size_t t = 0; t < fresh.scenes[s].tracks.size(); ++t)
                    CHECK(fresh.scenes[s].tracks[t].ampEnv == old.scenes[s].tracks[t].ampEnv);
                for (std::size_t q = 0; q < fresh.scenes[s].patterns.size(); ++q)
                {
                    INFO("scene " << s + 1 << " pattern " << q + 1);
                    const auto& a = fresh.scenes[s].patterns[q];
                    const auto& b = old.scenes[s].patterns[q];
                    CHECK(a.steps == b.steps);
                    CHECK(a.locks == b.locks);
                    CHECK(encodePattern(a, ParamIndexMap::builtin(), &fx) == encodePattern(b, ParamIndexMap::builtin(), &fx));
                    for (const auto& row : b.steps)
                        for (const auto& c : row)
                            cells += c.trigger != core::model::Step::kEmpty ? 1 : 0;
                }
            }
            std::printf("MIGRATE %s: %d programmed cells compared\n", bundle.getFileName().toRawUTF8(), cells);
        }
        CHECK(services::store::contentId(f).value() == before.value()); // FMT-045
        CHECK(f.getLastModificationTime() == mtime);
        ++checked;
    }
    if (checked == 0)
        SKIP("no private files in " << dir.getFullPathName().toStdString());
}
