// Sidecar metadata (LIB-023): the reference `.wav.dat` (JUCE binary ValueTree `SampleAdjunctData`, generated here with
// JUCE's own writer) is read, the app's `.wav.json` is written atomically and wins over the `.dat`.
#include "LibTestAudio.h"

#include "services/library/SidecarStore.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_data_structures/juce_data_structures.h>

using namespace services::library;

namespace
{
juce::MemoryBlock adjunct(double bar, double bpm, int sampleNum, const char* type = "SampleAdjunctData")
{
    juce::ValueTree t{juce::Identifier(type)};
    t.setProperty("bar", bar, nullptr);
    t.setProperty("bpm", bpm, nullptr);
    t.setProperty("sampleNum", sampleNum, nullptr);
    t.setProperty("note", "a string is skipped", nullptr);
    t.appendChild(juce::ValueTree{"Child"}, nullptr);
    juce::MemoryOutputStream out;
    t.writeToStream(out);
    return out.getMemoryBlock();
}
} // namespace

TEST_CASE("lib.sidecar: the reference .wav.dat is read", "[library][sidecar]")
{
    const auto data = adjunct(0.25, 0.0, 20671);
    const auto m = SidecarStore::parseReference(data.getData(), data.getSize());
    REQUIRE(m);
    CHECK(m->bars == 0.25);
    CHECK(m->bpm == 0.0);
    CHECK(m->sampleNumber == 20671);

    const auto other = adjunct(1, 120, 1, "SomethingElse");
    CHECK_FALSE(SidecarStore::parseReference(other.getData(), other.getSize()));
    CHECK_FALSE(SidecarStore::parseReference(data.getData(), data.getSize() - 3)); // cut short
    CHECK_FALSE(SidecarStore::parseReference(nullptr, 0));
}

TEST_CASE("lib.sidecar: .wav.json is written atomically, read back and preferred over .wav.dat", "[library][sidecar]")
{
    libtest::TempDir tmp("sidecar");
    const juce::File wav = tmp / "loop.wav";
    REQUIRE(wav.replaceWithText("x"));
    SidecarStore store;
    core::model::SampleMetadata none;
    CHECK_FALSE(store.tryRead(wav, none));

    const auto dat = adjunct(2, 98, 5);
    REQUIRE(SidecarStore::referenceFor(wav).replaceWithData(dat.getData(), dat.getSize()));
    CHECK(SidecarStore::referenceFor(wav).getFileName() == "loop.wav.dat");
    CHECK(store.read(wav).bpm == 98);

    core::model::SampleMetadata m;
    m.bars = 4, m.bpm = 127.5, m.sampleNumber = 9, m.peak = 0.5f;
    REQUIRE(store.write(wav, m).ok());
    CHECK(SidecarStore::jsonFor(wav).getFileName() == "loop.wav.json");
    CHECK(tmp.dir().findChildFiles(juce::File::findFiles, false, "*.tmp-*").isEmpty());
    const auto back = store.read(wav);
    CHECK(back.bars == 4.0);
    CHECK(back.bpm == 127.5);
    CHECK(back.sampleNumber == 9);
    m.bpm = 0; // a .json without tempo still wins
    REQUIRE(store.write(wav, m).ok());
    CHECK(store.read(wav).bpm == 0.0);
    CHECK(back.peak == 0.5f);
    CHECK(SidecarStore::referenceFor(wav).getSize() == static_cast<juce::int64>(dat.getSize())); // never written
}
