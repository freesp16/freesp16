// Container + tree codec (FMT-043; research docs/project-format.md "Container", "JUCE binary ValueTree encoding").
#include "services/interop/sp16/StepMatrix.h"
#include "services/interop/sp16/ValueTreeCodec.h"
#include "tests/unit/interop/Fixtures.h"

#include <juce_data_structures/juce_data_structures.h>

#include <catch2/catch_test_macros.hpp>

using namespace services::interop::sp16;

namespace
{
std::vector<std::byte> b(std::initializer_list<int> v)
{
    std::vector<std::byte> out;
    for (int x : v)
        out.push_back(static_cast<std::byte>(x));
    return out;
}

Node sample()
{
    Node n;
    n.type = "Root7";
    n.props = {{"i", Var::ofInt(-5)},           {"t", Var::ofBool(true)},  {"f", Var::ofBool(false)},
               {"d", Var::ofDouble(131.0)},     {"s", Var::ofString("ünï")}, {"l", Var::ofInt64(1ll << 40)},
               {"v", Var {}},                   {"bin", Var::ofBinary(b({1, 2, 3}))},
               {"a", Var::ofArray({Var::ofString("None"), Var::ofDouble(0.4), Var::ofArray({})})}};
    Node c;
    c.type = "Child";
    n.children = {c, c};
    return n;
}

// JUCE's own reader as the oracle (a cross-check; host only).
void sameAsJuce(const Node& n, const juce::ValueTree& t)
{
    INFO(n.type);
    REQUIRE(t.getType().toString().toStdString() == n.type);
    REQUIRE(t.getNumProperties() == static_cast<int>(n.props.size()));
    for (int k = 0; k < t.getNumProperties(); ++k)
    {
        const auto& [name, v] = n.props[static_cast<std::size_t>(k)];
        REQUIRE(t.getPropertyName(k).toString().toStdString() == name);
        const juce::var& jv = t.getProperty(t.getPropertyName(k));
        switch (v.t)
        {
            case Var::T::Int: CHECK((jv.isInt() && static_cast<int>(jv) == v.i)); break;
            case Var::T::Int64: CHECK((jv.isInt64() && static_cast<juce::int64>(jv) == v.i)); break;
            case Var::T::True:
            case Var::T::False: CHECK((jv.isBool() && static_cast<bool>(jv) == (v.t == Var::T::True))); break;
            case Var::T::Double: CHECK((jv.isDouble() && static_cast<double>(jv) == v.d)); break;
            case Var::T::String: CHECK(jv.toString().toStdString() == v.s); break;
            case Var::T::Array: CHECK((jv.isArray() && jv.getArray()->size() == static_cast<int>(v.a.size()))); break;
            case Var::T::Binary:
                CHECK((jv.isBinaryData() && jv.getBinaryData()->getSize() == v.bin.size()));
                break;
            case Var::T::Void: CHECK(jv.isVoid()); break;
            case Var::T::Undefined: CHECK(jv.isUndefined()); break;
        }
    }
    REQUIRE(t.getNumChildren() == static_cast<int>(n.children.size()));
    for (int k = 0; k < t.getNumChildren(); ++k)
        sameAsJuce(n.children[static_cast<std::size_t>(k)], t.getChild(k));
}
} // namespace

TEST_CASE("prj tree: compressed ints are minimal and keep the sign bit", "[interop]")
{
    Node n;
    n.type = "N";
    n.props = {{"a", Var::ofInt(0)}};
    const auto bytes = encodeTree(n);
    // "N\0" · count 1 (01 01) · "a\0" · var N=5 (01 05) · marker 1 · 00 00 00 00 · children 0 (00)
    CHECK(bytes == b({'N', 0, 1, 1, 'a', 0, 1, 5, 1, 0, 0, 0, 0, 0}));
    Node e;
    e.type = "E";
    CHECK(encodeTree(e) == b({'E', 0, 0, 0})); // 0 = the single byte 00
    // A negative compressed count is refused; a non-minimal one decodes and is counted.
    CHECK_FALSE(decodeTree(b({'E', 0, 0x81, 1, 0})));
    DecodeInfo info;
    auto r = decodeTree(b({'E', 0, 1, 0, 0}), {}, &info);
    REQUIRE(r);
    CHECK(info.nonMinimalInts == 1);
    CHECK_FALSE(decodeTree(b({'E', 0, 5, 0, 0, 0, 0, 0, 0}))); // length byte > 4
}

TEST_CASE("prj tree: every var type round-trips byte for byte and matches JUCE's reader", "[interop]")
{
    const Node n = sample();
    const auto bytes = encodeTree(n);
    auto back = decodeTree(bytes);
    REQUIRE(back);
    CHECK(back.value() == n);
    CHECK(encodeTree(back.value()) == bytes);
    // Strings carry their NUL like JUCE's writer; a payload without one is kept as such.
    Node u = n;
    u.props[4].second.unterminated = true;
    auto ub = decodeTree(encodeTree(u));
    REQUIRE(ub);
    CHECK(ub.value().props[4].second.unterminated);
    CHECK(encodeTree(ub.value()) == encodeTree(u));

    sameAsJuce(n, juce::ValueTree::readFromData(bytes.data(), bytes.size()));
    // And JUCE's writer gives our bytes for the same tree.
    juce::MemoryOutputStream mo;
    juce::ValueTree::readFromData(bytes.data(), bytes.size()).writeToStream(mo);
    CHECK(std::vector<std::byte>(static_cast<const std::byte*>(mo.getData()),
                                 static_cast<const std::byte*>(mo.getData()) + mo.getDataSize()) == bytes);
}

TEST_CASE("prj tree: suffixes", "[interop]")
{
    CHECK(splitSuffix("SceneData15") == std::pair<std::string_view, std::optional<int>> {"SceneData", 15});
    CHECK(splitSuffix("PlaybackData").second == std::nullopt);
    CHECK(splitSuffix("MidiCc1Data").second == std::nullopt); // digits inside the name are not a suffix
    CHECK(splitSuffix("7").second == std::nullopt);
}

TEST_CASE("prj tree: limits and damage are errors, never a crash or a short tree", "[interop]")
{
    const auto good = encodeTree(sample());
    for (std::size_t cut = 0; cut < good.size(); ++cut)
        CHECK_FALSE(decodeTree(std::span(good).first(cut)));
    auto trailing = good;
    trailing.push_back(std::byte {0});
    CHECK_FALSE(decodeTree(trailing));
    // Depth: a chain of 100 nodes is refused with the default limit of 64.
    Node deep;
    deep.type = "D";
    for (int k = 0; k < 100; ++k)
    {
        Node up;
        up.type = "D";
        up.children.push_back(std::move(deep));
        deep = std::move(up);
    }
    const auto deepBytes = encodeTree(deep);
    auto r = decodeTree(deepBytes);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("deeper") != std::string::npos);
    Limits loose;
    loose.maxDepth = 200;
    CHECK(decodeTree(deepBytes, loose));
    // A child count of 2^31 − 1 with two bytes left.
    CHECK_FALSE(decodeTree(b({'E', 0, 0, 4, 0xff, 0xff, 0xff, 0x7f, 0, 0})));
    // Unknown var marker.
    CHECK_FALSE(decodeTree(b({'E', 0, 1, 'x', 0, 1, 0x0b, 0})));
}

TEST_CASE("prj container: zlib header, checksum and size cap", "[interop]")
{
    const Node n = sample();
    const auto c = writeContainer(n);
    REQUIRE(hasContainerMagic(c));
    CHECK(static_cast<int>(c[8]) == 0x78);
    CHECK(static_cast<int>(c[9]) == 0x01); // outer level 1
    auto back = readContainer(c);
    REQUIRE(back);
    CHECK(back.value() == n);
    auto damaged = c;
    damaged[c.size() - 1] ^= std::byte {1}; // Adler-32
    CHECK_FALSE(readContainer(damaged));
    CHECK_FALSE(readContainer(std::span(c).first(c.size() - 5)));
    // A zip bomb: 64 MB of zeros (a ~64 KB stream) against a 1 MB cap.
    std::vector<std::byte> zeros(64u << 20);
    const auto bomb = deflateZlib(zeros, 9);
    auto capped = inflateZlib(bomb, 1u << 20);
    REQUIRE_FALSE(capped);
    CHECK(capped.error().message.find("expands") != std::string::npos);
    CHECK(deflateZlib(asBytes("x"), 6)[1] == std::byte {0x9c});
}

TEST_CASE("prj container: synthetic projects round-trip byte-identical (FMT-043) and match JUCE", "[interop]")
{
    for (const char* name : {"basic", "full", "sparse", "observed"})
    {
        INTEROP_FIXTURE(g, name);
        INFO(name);
        const auto bytes = interop_test::bytesOf(g.prj);
        DecodeInfo info;
        std::size_t inflated = 0;
        auto tree = readContainer(bytes, {}, &info, &inflated);
        REQUIRE(tree);
        CHECK(info.nonMinimalInts == 0);
        // The fixture was written by an independent encoder (Python's zlib): the bundled zlib must reproduce it. If
        // this fails only on the zlib bytes, the JUCE zlib build changed (it is pinned with the JUCE submodule).
        const auto again = writeContainer(tree.value());
        CHECK(again.size() == bytes.size());
        CHECK(again == bytes);
        auto inner = inflateZlib(std::span(bytes).subspan(8), 64u << 20);
        REQUIRE(inner);
        CHECK(encodeTree(tree.value()) == inner.value());
        sameAsJuce(tree.value(), juce::ValueTree::readFromData(inner.value().data(), inner.value().size()));
        // Every inner blob too: Steps and Sections at level 6.
        std::function<void(const Node&)> blobs = [&](const Node& n)
        {
            if (const auto* s = n.prop("Steps"); s != nullptr && s->t == Var::T::Binary)
            {
                auto rec = decodeSteps(s->bin);
                REQUIRE(rec);
                CHECK(encodeSteps(rec.value()) == s->bin);
            }
            if (const auto* s = n.prop("Sections"); s != nullptr && s->t == Var::T::Binary)
            {
                auto rec = decodeSections(s->bin);
                REQUIRE(rec);
                CHECK(rec.value().size() == 999u);
                CHECK(encodeSections(rec.value()) == s->bin);
            }
            for (const auto& c : n.children)
                blobs(c);
        };
        blobs(tree.value());
    }
}
