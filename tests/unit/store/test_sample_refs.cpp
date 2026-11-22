// Sample references (FMT-011, FMT-012, PRJ-014, PRJ-040/041): content ids, locations, safe resolution, Collect & Save.
#include "tests/unit/store/StoreTestSupport.h"

#include "services/store/CodecRegistry.h"
#include "services/store/SampleRefs.h"

#include <catch2/catch_test_macros.hpp>

using namespace services::store;
using storetest::ScratchFolder;

TEST_CASE("store: SHA-256 matches the FIPS 180-4 test vectors", "[store][samples]")
{
    // SHA-256 vectors (FIPS 180-4 / NIST CAVS); each expected line names sha256 for tools/check-no-firmware.sh
    CHECK(contentIdOf("", 0) == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(contentIdOf("abc", 3) == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(contentIdOf(two.data(), two.size()) ==
          "sha256:248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // streaming in odd chunks gives the same digest as one call
    const std::string million(1000000, 'a');
    Sha256 sha;
    for (std::size_t i = 0; i < million.size(); i += 777)
        sha.update(million.data() + i, std::min<std::size_t>(777, million.size() - i));
    CHECK("sha256:" + Sha256::hex(sha.finish()) ==
          "sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(isContentId(contentIdOf("abc", 3)));
    CHECK_FALSE(isContentId("sha256:ABC"));

    ScratchFolder dir("sha");
    REQUIRE((dir / "abc.wav").replaceWithText("abc"));
    const auto id = contentId(dir / "abc.wav");
    REQUIRE(id.ok());
    CHECK(id.value() == contentIdOf("abc", 3));
    CHECK_FALSE(contentId(dir / "missing.wav").ok());
}

TEST_CASE("store: locations split and join exactly", "[store][samples]")
{
    for (const char* s : {"$Lib/Drums/kick.wav", "$Bundle/a.wav", "$Ext:USB1/loops/a.wav", "/abs/x.wav", "$Lib",
                          "$Lib/", "$Ext:/x.wav", "$Ext:name", "relative.wav", "", "$Libx/y"})
    {
        INFO(s);
        const auto l = splitLocation(s);
        CHECK(joinLocation(l) == s);
        const auto back = parseLocationTag(locationTag(l), l.path);
        REQUIRE(back.has_value());
        CHECK(*back == l);
    }
    CHECK(splitLocation("$Ext:USB1/loops/a.wav").volume == "USB1");
    CHECK(splitLocation("$Lib/Drums/kick.wav").kind == LocationKind::Library);
    CHECK(splitLocation("$Lib").kind == LocationKind::Absolute);
    CHECK_FALSE(parseLocationTag("$Cloud", "x").has_value());
}

TEST_CASE("store: resolution never leaves its root and marks missing files", "[store][samples]")
{
    ScratchFolder dir("resolve");
    SampleRoots roots;
    roots.library = dir / "Library";
    roots.bundle = dir / "Song.sixteen";
    roots.externals["USB1"] = dir / "usb";
    REQUIRE(roots.library.getChildFile("Drums/kick.wav").create().wasOk());

    CHECK(resolveLocation(splitLocation("$Lib/Drums/kick.wav"), roots) == roots.library.getChildFile("Drums/kick.wav"));
    CHECK(resolveLocation(splitLocation("$Bundle/a.wav"), roots) == roots.bundle.getChildFile("samples/a.wav"));
    CHECK(resolveLocation(splitLocation("$Ext:USB1/x.wav"), roots) == (dir / "usb").getChildFile("x.wav"));
    for (const char* unsafe : {"$Lib/../secret.wav", "$Lib/a/../../b.wav", "$Bundle/./a.wav", "$Lib/a//b.wav",
                               "$Ext:USB2/x.wav", "relative.wav"})
    {
        INFO(unsafe);
        CHECK_FALSE(resolveLocation(splitLocation(unsafe), roots).has_value());
    }

    core::model::SamplePool pool;
    pool.entries.push_back({"sha256:1", "$Lib/Drums/kick.wav", "kick", true, {}});
    pool.entries.push_back({"sha256:2", "$Lib/Drums/snare.wav", "snare", false, {}});
    CHECK(resolvePool(pool, roots) == 1);
    CHECK_FALSE(pool.entries[0].missing);
    CHECK(pool.entries[1].missing);
}

TEST_CASE("store: safe file names fit FAT32", "[store][samples]")
{
    CHECK(safeFileName("kick.wav") == "kick.wav");
    CHECK(safeFileName("a/b\\c:d*e?.wav") == "a_b_c_d_e_.wav");
    CHECK(safeFileName("  .hidden. ") == "hidden");
    CHECK(safeFileName("") == "sample");
    const auto longName = safeFileName(std::string(100, 'x') + ".wav");
    CHECK(longName.size() == 64);
    CHECK(longName.substr(60) == ".wav");
    const auto utf8 = safeFileName(std::string(70, 'x') + "ä"); // never cut inside a UTF-8 sequence
    CHECK(utf8.size() <= 64);
    CHECK(juce::String::fromUTF8(utf8.c_str()).toStdString() == utf8);
}

TEST_CASE("store: Collect & Save copies samples into the bundle and rewrites their locations", "[store][samples]")
{
    ScratchFolder dir("collect");
    SaveOptions options;
    options.collectSamples = true;
    options.roots.library = dir / "Library";
    REQUIRE(options.roots.library.getChildFile("Drums").createDirectory().wasOk());
    REQUIRE(options.roots.library.getChildFile("Other").createDirectory().wasOk());
    REQUIRE(options.roots.library.getChildFile("Drums/kick.wav").replaceWithText("kick-data"));
    REQUIRE(options.roots.library.getChildFile("Other/kick.wav").replaceWithText("other-kick"));

    auto p = core::model::newDefaultProject();
    p->sampleRefs.entries.push_back({contentIdOf("kick-data", 9), "$Lib/Drums/kick.wav", "kick", false, {}});
    p->sampleRefs.entries.push_back({contentIdOf("other-kick", 10), "$Lib/Other/kick.wav", "kick 2", false, {}});
    p->sampleRefs.entries.push_back({"sha256:gone", "$Lib/missing.wav", "gone", false, {}});
    const auto bundle = dir / "Collected.sixteen";
    SaveReport report;
    REQUIRE(saveProject(*p, bundle, options, &report).ok());

    REQUIRE(report.pool.entries.size() == 3);
    CHECK(report.pool.entries[0].path == "$Bundle/kick.wav");
    CHECK(report.pool.entries[1].path == "$Bundle/kick-2.wav");
    CHECK(report.pool.entries[2].path == "$Lib/missing.wav");
    CHECK(report.warnings.size() == 1);
    CHECK(bundle.getChildFile("samples/kick.wav").loadFileAsString() == "kick-data");
    CHECK(bundle.getChildFile("samples/kick-2.wav").loadFileAsString() == "other-kick");
    CHECK(options.roots.library.getChildFile("Drums/kick.wav").existsAsFile()); // PRJ-014: copied, not moved

    auto opened = openProject(bundle);
    REQUIRE(opened.ok());
    CHECK(opened.value()->sampleRefs == report.pool);
    // a later plain save keeps the collected files (they are carried over)
    REQUIRE(saveProject(*opened.value(), bundle).ok());
    CHECK(bundle.getChildFile("samples/kick-2.wav").loadFileAsString() == "other-kick");
    SampleRoots roots;
    roots.bundle = bundle;
    CHECK(resolvePool(opened.value()->sampleRefs, roots) == 1); // only the missing one
}
