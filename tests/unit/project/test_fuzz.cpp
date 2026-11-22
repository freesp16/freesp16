// NFR-011: a deterministic 2,000-case mutation corpus of the project inputs never crashes (run under ASan/UBSan in the
// host preset). Every input is generated here from a default or a seeded random project (no device data):
// 1,500 bundle files (project.json, meta.json, a pattern file) through the codec pipeline from memory, 300 autosave
// copies, and 200 damaged bundles on disk through ProjectStore::open (recovery, resolvePool, the sample pool).
#include "ProjectTestSupport.h"
#include "fuzz/FuzzTargets.h"

#include <catch2/catch_test_macros.hpp>

using namespace projtest;

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

std::string mutate(std::string s, Rng& r)
{
    static const char* tokens[] = {"null", "-1", "1e309", "-1e309", "999999999999", "\"x\"", "[]", "{}", "true", ",",
                                   ":", "\"\\u0000\"", "-0", "NaN", "[[[[[[[[", "}}}}", "\"\\ud800\"", "0.5",
                                   "4294967296", "-2147483649", "\"$Lib/../../etc/passwd\"", "\"sha256:zz\""};
    const int edits = 1 + static_cast<int>(r.below(4));
    for (int i = 0; i < edits; ++i)
    {
        const std::size_t at = r.below(s.size() + 1);
        switch (r.below(8))
        {
        case 0:
            if (! s.empty())
                s[r.below(s.size())] = static_cast<char>(r.next());
            break;
        case 1: s.resize(at); break;
        case 2: s.insert(at, tokens[r.below(std::size(tokens))]); break;
        case 3: // a number becomes an extreme
        {
            const auto d = s.find_first_of("0123456789", at);
            if (d == std::string::npos)
                break;
            auto e = s.find_first_not_of("0123456789.eE+-", d);
            e = e == std::string::npos ? s.size() : e;
            static const char* extremes[] = {"-2147483649", "4294967296", "1e308", "-1e308", "0.000001", "65", "-65",
                                             "1000000", "17", "-1"};
            s.replace(d, e - d, extremes[r.below(std::size(extremes))]);
            break;
        }
        case 4: s.erase(at, r.below(64)); break;
        case 5:
            if (at < s.size())
                s.insert(r.below(s.size()), s.substr(at, r.below(256)));
            break;
        case 6: // a string value becomes long or odd
        {
            const auto q = s.find('"', at);
            if (q == std::string::npos)
                break;
            const auto q2 = s.find('"', q + 1);
            if (q2 == std::string::npos)
                break;
            static const std::string longText(5000, 'w');
            const std::string odd[] = {longText, "Größe 日本", "", "Off", "Sample", "Reverb", "$Bundle/x.wav", "1"};
            s.replace(q + 1, q2 - q - 1, odd[r.below(std::size(odd))]);
            break;
        }
        default: // deep nesting (a stack bomb for recursive parsers)
            s.insert(at, std::string(r.below(4) == 0 ? 100000 : 300, r.below(2) ? '[' : '{'));
            break;
        }
    }
    return s;
}

const std::uint8_t* bytes(const std::string& s) { return reinterpret_cast<const std::uint8_t*>(s.data()); }
} // namespace

TEST_CASE("project.fuzz: 2,000 mutated project inputs never crash", "[project][fuzz]")
{
    using projfuzz::BundleFile;
    // Bases: the default files and a rich random project's files.
    st::SaveOptions o;
    o.timestamp = "2026-01-01T00:00:00.000Z";
    const auto rich = storetest::randomProject(77, 0.15);
    std::string richProject, richMeta, richScene;
    for (auto& f : st::NativeCodec::encode(*rich, o))
    {
        if (f.name == "project.json")
            richProject = f.text;
        else if (f.name == "meta.json")
            richMeta = f.text;
        else if (f.name == "patterns/scene-01.json")
            richScene = f.text;
    }
    // Every scene of the rich project has content in scene 1 only by chance: make sure scene 1 has steps.
    if (richScene.size() < 200)
        richScene = projfuzz::defaultFile(BundleFile::Scene);

    Rng rng {20261121};
    int cases = 0;
    for (int i = 0; i < 1500; ++i, ++cases)
    {
        const auto pick = rng.below(10);
        const BundleFile which = pick < 6 ? BundleFile::Project : pick < 8 ? BundleFile::Meta : BundleFile::Scene;
        const bool useRich = rng.below(2) == 0;
        const std::string& base = which == BundleFile::Project ? (useRich ? richProject : projfuzz::defaultFile(which))
                                  : which == BundleFile::Meta  ? (useRich ? richMeta : projfuzz::defaultFile(which))
                                                               : (useRich ? richScene : projfuzz::defaultFile(which));
        const auto text = mutate(base, rng);
        projfuzz::bundleFile(which, bytes(text), text.size());
    }

    const auto autosaveBase = st::Autosave::encode(*rich, "Fuzz", juce::File("/tmp/Fuzz.sixteen"), o.timestamp);
    for (int i = 0; i < 300; ++i, ++cases)
    {
        const auto text = mutate(autosaveBase, rng);
        projfuzz::autosave(bytes(text), text.size());
    }

    // On disk, through the store (recovery, nesting/size guards, resolvePool, pool requests).
    Env env("fuzz");
    env.startup();
    const auto bundle = env.projects().getChildFile("Fuzz.sixteen");
    int opened = 0, refused = 0;
    for (int i = 0; i < 200; ++i, ++cases)
    {
        bundle.deleteRecursively();
        REQUIRE(st::saveProject(*rich, bundle).ok());
        const auto kind = rng.below(10);
        if (kind < 6)
        {
            const char* names[] = {"project.json", "meta.json", "patterns/scene-01.json", "patterns/scene-16.json"};
            const auto f = bundle.getChildFile(names[rng.below(4)]);
            REQUIRE(storetest::writeBytes(f, mutate(storetest::readBytes(f), rng)));
        }
        else if (kind == 6)
            bundle.getChildFile(rng.below(2) ? "meta.json" : "patterns/scene-03.json").deleteFile();
        else if (kind == 7)
        {
            bundle.getChildFile("project.json").deleteFile();
            bundle.getChildFile("project.json").createDirectory();
        }
        else if (kind == 8) // leftovers of an interrupted save next to it
            REQUIRE(bundle.copyDirectoryTo(st::AtomicWriter::sealedOf(bundle)));
        else
            REQUIRE(storetest::writeBytes(bundle.getChildFile("project.json"), std::string()));
        st::LoadReport report;
        const auto r = env.open(bundle, &report);
        (r.ok() ? opened : refused)++;
        if (r.ok())
        {
            std::vector<std::string> problems;
            (void) m::validate(env.project(), problems, 10);
        }
        else
            CHECK_FALSE(r.error().message.empty());
        CHECK_FALSE(env.store().busy());
    }
    CHECK(cases == 2000);
    CHECK(opened > 0);
    CHECK(refused > 0);
}

TEST_CASE("project.fuzz: audio and sidecar entry points accept garbage", "[project][fuzz]")
{
    Rng rng {7};
    for (int i = 0; i < 64; ++i)
    {
        std::string junk(rng.below(512), '\0');
        for (auto& c : junk)
            c = static_cast<char>(rng.next());
        if (i % 4 == 0)
            junk.replace(0, std::min<std::size_t>(4, junk.size()), "RIFF");
        projfuzz::audio(bytes(junk), junk.size());
        projfuzz::sidecar(bytes(junk), junk.size());
    }
    SUCCEED();
}
