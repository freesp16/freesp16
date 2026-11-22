// Your own files, local only (label interop-private through tests/unit/interop-private/test_private_projects.py):
// every .prj / .rsm in tests/interop/private/ (git-ignored, never in CI) must round-trip byte-identical at tree level
// (FMT-043), import without an error and leave the files untouched (FMT-045); the import report is printed for
// sending back. Every .wav.dat there must parse. Hidden ([.private]): skipped when the folder is empty.
#include "services/interop/sp16/Sp16ProjectCodec.h"
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
        CHECK(services::store::contentId(f).value() == before.value()); // FMT-045
        CHECK(f.getLastModificationTime() == mtime);
        ++checked;
    }
    if (checked == 0)
        SKIP("no private files in " << dir.getFullPathName().toStdString());
}
