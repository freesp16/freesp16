// A small JSON reader and writer for the controller profiles (io links no JUCE). RFC 8259 values
// (objects keep their member order, duplicate keys keep the first), \uXXXX escapes with surrogate pairs, numbers as
// double; depth ≤ 32. Message thread only (profile load, MIDI learn save): allocates freely, never on a reader thread.
#pragma once

#include "core/util/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace io::controllers::json
{
struct Value
{
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;      // Array elements, or Object member values
    std::vector<std::string> keys; // Object member names (index-aligned with items)

    bool isNull() const noexcept { return type == Type::Null; }
    bool isBool() const noexcept { return type == Type::Bool; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isString() const noexcept { return type == Type::String; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isObject() const noexcept { return type == Type::Object; }
    // Object member (null when absent or not an object).
    const Value* get(std::string_view key) const noexcept;
};

core::Result<Value> parse(std::string_view text);

// Writer helpers: a JSON string literal (quotes and escapes) and a compact number.
std::string quote(std::string_view text);
std::string number(double v);
} // namespace io::controllers::json
