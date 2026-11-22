// NFR-011 for the reference importer: generated corrupt variants of the synthetic fixtures (byte flips, truncation,
// insertions, length-field damage, type swaps in the tree, damaged step matrices and sidecars) must give a Result
// error or a valid model — never a crash, a hang or an unbounded allocation. Deterministic (fixed seeds); the time per
// target can be raised for a local soak: INTEROP_FUZZ_SECONDS=600 (10 min) — default a few seconds.
#include "core/model/ModelOps.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using namespace services::interop::sp16;

namespace
{
double budgetSeconds(double fallback)
{
    if (const char* s = std::getenv("INTEROP_FUZZ_SECONDS"))
        return std::max(0.1, std::atof(s));
    return fallback;
}

struct Clock
{
    double end;
    explicit Clock(double seconds) : end(juce::Time::getMillisecondCounterHiRes() + 1000.0 * seconds) {}
    bool running() const { return juce::Time::getMillisecondCounterHiRes() < end; }
};

void mutate(std::vector<std::byte>& d, juce::Random& rnd)
{
    if (d.empty())
    {
        d.push_back(static_cast<std::byte>(rnd.nextInt(256)));
        return;
    }
    const int kind = rnd.nextInt(6);
    const auto at = static_cast<std::size_t>(rnd.nextInt(static_cast<int>(d.size())));
    switch (kind)
    {
        case 0: // flip a few bits
            for (int k = 0, n = 1 + rnd.nextInt(8); k < n; ++k)
                d[static_cast<std::size_t>(rnd.nextInt(static_cast<int>(d.size())))] ^=
                    static_cast<std::byte>(1 << rnd.nextInt(8));
            break;
        case 1: d.resize(at); break; // truncate
        case 2: // insert random bytes
            for (int k = 0, n = 1 + rnd.nextInt(16); k < n; ++k)
                d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::byte>(rnd.nextInt(256)));
            break;
        case 3: // an extreme compressed int / length byte
            d[at] = static_cast<std::byte>(rnd.nextBool() ? 0x84 : (rnd.nextBool() ? 0x04 : 0xff));
            if (at + 4 < d.size())
                for (int k = 1; k <= 4; ++k)
                    d[at + static_cast<std::size_t>(k)] = std::byte {0xff};
            break;
        case 4: // duplicate a chunk
        {
            const auto n = std::min<std::size_t>(d.size() - at, 1 + static_cast<std::size_t>(rnd.nextInt(64)));
            std::vector<std::byte> chunk(d.begin() + static_cast<std::ptrdiff_t>(at),
                                         d.begin() + static_cast<std::ptrdiff_t>(at + n));
            d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
            break;
        }
        default: d[at] = std::byte {0}; break; // a NUL where a name was
    }
}

// Tree-level damage the byte mutator rarely reaches: wrong var types, missing / duplicate / renamed nodes.
void mutateTree(Node& n, juce::Random& rnd, int depth = 0)
{
    if (! n.props.empty() && rnd.nextInt(4) == 0)
    {
        auto& v = n.props[static_cast<std::size_t>(rnd.nextInt(static_cast<int>(n.props.size())))].second;
        switch (rnd.nextInt(7))
        {
            case 0: v = Var::ofString("?" + std::to_string(rnd.nextInt())); break;
            case 1: v = Var::ofInt(rnd.nextInt()); break;
            case 2: v = Var::ofDouble(rnd.nextBool() ? 1e300 : -1e300); break;
            case 3: v = Var {}; break;
            case 4: v = Var::ofArray({Var::ofInt(1), Var::ofArray({})}); break;
            case 5: v = Var::ofBinary(std::vector<std::byte>(static_cast<std::size_t>(rnd.nextInt(64)))); break;
            default: v = Var::ofInt64(static_cast<std::int64_t>(rnd.nextInt64())); break;
        }
    }
    if (! n.children.empty() && rnd.nextInt(16) == 0)
    {
        const auto k = static_cast<std::size_t>(rnd.nextInt(static_cast<int>(n.children.size())));
        switch (rnd.nextInt(3))
        {
            case 0: n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(k)); break;
            case 1: n.children.push_back(n.children[k]); break;
            default: n.children[k].type += std::to_string(rnd.nextInt(300)); break;
        }
    }
    if (depth < 6)
        for (auto& c : n.children)
            if (rnd.nextInt(3) == 0)
                mutateTree(c, rnd, depth + 1);
}

int mapOne(const Node& tree)
{
    auto p = core::model::newDefaultProject();
    ImportLog log;
    MapOptions mo; // no resolver: every sample missing
    return mapProject(tree, *p, mo, log) ? 1 : 0;
}
} // namespace

TEST_CASE("fuzz prj_container: damaged container bytes", "[interop][fuzz]")
{
    INTEROP_FIXTURE(g, "full");
    const auto seed = interop_test::bytesOf(g.prj);
    juce::Random rnd(1234);
    Clock clock(budgetSeconds(3.0));
    int runs = 0, decoded = 0;
    while (clock.running() || runs < 200)
    {
        auto d = seed;
        for (int k = 0, n = 1 + rnd.nextInt(3); k < n; ++k)
            mutate(d, rnd);
        auto tree = readContainer(d);
        if (tree)
            decoded += mapOne(tree.value());
        ++runs;
    }
    INFO("runs " << runs << ", decoded " << decoded);
    CHECK(runs >= 200);
}

TEST_CASE("fuzz prj_tree: damaged inflated trees, re-wrapped, then mapped", "[interop][fuzz]")
{
    INTEROP_FIXTURE(g, "basic");
    auto base = readContainer(interop_test::bytesOf(g.prj));
    REQUIRE(base);
    const auto tree = encodeTree(base.value());
    juce::Random rnd(99);
    Clock clock(budgetSeconds(3.0));
    int runs = 0, mapped = 0;
    while (clock.running() || runs < 50)
    {
        if (rnd.nextBool())
        {
            auto d = tree;
            mutate(d, rnd);
            auto t = decodeTree(d);
            if (t)
                mapped += mapOne(t.value());
        }
        else
        {
            Node n = base.value();
            mutateTree(n, rnd);
            mapped += mapOne(n);
            auto again = decodeTree(encodeTree(n)); // the encoder accepts whatever the mapper survived
            CHECK(again);
        }
        ++runs;
    }
    INFO("runs " << runs << ", mapped " << mapped);
    CHECK(mapped > 0);
}

TEST_CASE("fuzz prj_steps: damaged step matrices and sections", "[interop][fuzz]")
{
    std::vector<StepRecord> recs(1024, StepRecord::emptyRecord());
    recs[17].trigger = 100;
    recs[17].slots[4] = LockWord::unpack(0x00123404u);
    const auto raw = encodeStepsRaw(recs);
    juce::Random rnd(7);
    Clock clock(budgetSeconds(3.0));
    int runs = 0;
    while (clock.running() || runs < 100)
    {
        auto d = raw;
        if (rnd.nextInt(4) == 0)
            mutate(d, rnd); // sizes change too
        else
            for (int k = 0, n = 1 + rnd.nextInt(64); k < n; ++k) // in-place word damage keeps the size
                d[static_cast<std::size_t>(rnd.nextInt(static_cast<int>(d.size())))] =
                    static_cast<std::byte>(rnd.nextInt(256));
        Node pat;
        pat.type = "PatternData" + std::to_string(rnd.nextInt(40));
        pat.props = {{"Steps", Var::ofBinary(deflateZlib(d, 6))}, {"Length", Var::ofInt(rnd.nextInt(200) - 50)}};
        Node scene;
        scene.type = "SceneData" + std::to_string(rnd.nextInt(18));
        scene.children = {pat};
        Node root;
        root.type = "ProjectData";
        root.children = {scene};
        CHECK(mapOne(root) == 1);
        auto sections = decodeSections(deflateZlib(std::span(d).first(std::min<std::size_t>(d.size(), 7992)), 6));
        (void) sections;
        auto bad = deflateZlib(d, 6);
        mutate(bad, rnd);
        (void) decodeSteps(bad);
        ++runs;
    }
    CHECK(runs >= 100);
}

TEST_CASE("fuzz prj_wavdat: damaged sidecars", "[interop][fuzz]")
{
    INTEROP_FIXTURE(g, "basic");
    const auto seed = interop_test::bytesOf(g.diskRoot.getChildFile("Samples/kick.wav.dat"));
    REQUIRE(parseWavDat(seed));
    juce::Random rnd(5);
    Clock clock(budgetSeconds(1.0));
    int runs = 0;
    while (clock.running() || runs < 1000)
    {
        auto d = seed;
        for (int k = 0, n = 1 + rnd.nextInt(4); k < n; ++k)
            mutate(d, rnd);
        (void) parseWavDat(d);
        ++runs;
    }
    CHECK(runs >= 1000);
}
