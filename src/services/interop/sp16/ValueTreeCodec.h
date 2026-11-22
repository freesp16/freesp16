// The reference project container and its tree encoding (spec 04-file-formats/sp16-interop.md
// "Container", "ValueTree encoding"; research docs/project-format.md "Container" and "JUCE binary ValueTree encoding",
// CC BY 4.0). Own decoder/encoder written from those pages — not juce::ValueTree — so property order, `var` types and
// every byte survive a round trip (FMT-043: writeContainer(readContainer(x)) == x for files the reference wrote).
//
//   container   "vltrgzip" (8 bytes) + zlib stream (outer level 1, 78 01) → the tree
//   node        type (UTF-8, NUL-terminated) · property count (compressed int) · properties (name NUL-terminated +
//               var) · child count (compressed int) · children
//   compressed  1 length byte (bit 7 = negative, length ≤ 4) + that many little-endian bytes; minimal when written
//   var         compressed int N (= 1 + payload); N = 0 void; else a marker byte and N − 1 payload bytes: 1 int (4 B),
//               2 / 3 true / false, 4 double (8 B), 5 string (UTF-8 + NUL), 6 int64 (8 B), 7 array (compressed count +
//               vars), 8 binary, 9 undefined (JUCE's own marker, no payload; not in the research table, kept as is)
//
// Bounded parsing (NFR-011): every count is checked against the bytes left and the Limits; recursion depth (nodes and
// nested arrays together) is capped; inflate stops at Limits::maxInflated (zip bombs); the zlib stream's Adler-32 is
// checked, so a truncated or damaged stream is an error, never a short tree. Every failure is a Result error, never an
// exception. Worker threads only (never the audio thread).
#pragma once

#include "core/util/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace services::interop::sp16
{
using ::core::Result;

struct Var
{
    enum class T : std::uint8_t { Void, Int, True, False, Double, String, Int64, Array, Binary, Undefined };
    T t = T::Void;
    std::int64_t i = 0;             // Int, Int64
    double d = 0;                   // Double
    std::string s;                  // String (without the terminating NUL)
    std::vector<Var> a;             // Array
    std::vector<std::byte> bin;     // Binary
    bool unterminated = false;      // String: the payload had no trailing NUL (kept for a byte-exact round trip)

    static Var ofInt(std::int32_t v) { Var x; x.t = T::Int; x.i = v; return x; }
    static Var ofBool(bool v) { Var x; x.t = v ? T::True : T::False; return x; }
    static Var ofDouble(double v) { Var x; x.t = T::Double; x.d = v; return x; }
    static Var ofString(std::string v) { Var x; x.t = T::String; x.s = std::move(v); return x; }
    static Var ofInt64(std::int64_t v) { Var x; x.t = T::Int64; x.i = v; return x; }
    static Var ofArray(std::vector<Var> v) { Var x; x.t = T::Array; x.a = std::move(v); return x; }
    static Var ofBinary(std::vector<std::byte> v) { Var x; x.t = T::Binary; x.bin = std::move(v); return x; }

    bool isNumber() const noexcept { return t == T::Int || t == T::Int64 || t == T::Double; }
    bool isBool() const noexcept { return t == T::True || t == T::False; }
    double number() const noexcept { return t == T::Double ? d : static_cast<double>(i); }
    friend bool operator== (const Var&, const Var&) = default;
};

std::string_view typeName(Var::T) noexcept; // "int", "bool", "double", "string", "int64", "array", "binary", …

struct Node
{
    std::string type;
    std::vector<std::pair<std::string, Var>> props;
    std::vector<Node> children;

    const Var* prop(std::string_view name) const noexcept;
    friend bool operator== (const Node&, const Node&) = default;
};

// "SceneData3" → {"SceneData", 3}; "PlaybackData" → {"PlaybackData", nullopt}. Suffix digits only (≤ 9 of them).
std::pair<std::string_view, std::optional<int>> splitSuffix(std::string_view type) noexcept;

struct Limits
{
    std::size_t maxInflated = 64u << 20; // bytes after inflate (a real 756 KB project inflates to a few MB)
    std::size_t maxDepth = 64;           // nested nodes + nested arrays (real projects: 5)
    std::size_t maxChildren = 4096;      // children, properties or array elements of one node / array
    std::size_t maxString = 1u << 20;    // type / property names and string values
};

// What a decode saw besides the tree (round-trip diagnostics).
struct DecodeInfo
{
    std::size_t nodes = 0, props = 0, maxDepth = 0;
    std::size_t nonMinimalInts = 0; // compressed ints not in minimal form (the encoder writes minimal ones)
};

constexpr std::string_view kContainerMagic = "vltrgzip";
constexpr int kOuterLevel = 1, kInnerLevel = 6; // zlib levels (research "Tooling": byte-identical with these)

// One tree from `bytes`, which must be consumed exactly (trailing bytes are an error).
Result<Node> decodeTree(std::span<const std::byte> bytes, const Limits& = {}, DecodeInfo* = nullptr);
std::vector<std::byte> encodeTree(const Node&); // minimal compressed ints, strings NUL-terminated

bool hasContainerMagic(std::span<const std::byte> bytes) noexcept;
Result<Node> readContainer(std::span<const std::byte> bytes, const Limits& = {}, DecodeInfo* = nullptr,
                           std::size_t* inflatedSize = nullptr);
std::vector<std::byte> writeContainer(const Node&); // magic + zlib level 1

// zlib (RFC 1950) through JUCE's bundled zlib: inflate the whole of `in` (header checked, Adler-32 verified, at most
// maxOut bytes, error otherwise); deflate at `level` with JUCE's defaults (window 15, memLevel 8).
Result<std::vector<std::byte>> inflateZlib(std::span<const std::byte> in, std::size_t maxOut);
std::vector<std::byte> deflateZlib(std::span<const std::byte> in, int level);
std::uint32_t adler32(std::span<const std::byte>) noexcept;

// Small helpers shared by the codec and the tools/tests.
std::span<const std::byte> asBytes(std::string_view) noexcept;
std::string toBase64(std::span<const std::byte>);
std::optional<std::vector<std::byte>> fromBase64(std::string_view);
// A JSON form of a var that keeps its type ({"int":5}, {"string":"x"}, {"binary":"<base64>"}, …): used for values
// kept in model Extras (the importer's opaque data) and by the dump tool.
std::string varToJson(const Var&);
} // namespace services::interop::sp16
