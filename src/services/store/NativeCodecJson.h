// JSON text output of the native codec (internal to services/store). A streaming writer instead of juce::var +
// juce::JSON::toString, for two reasons:
//   - exact round trips: doubles are written in their shortest round-trip form (std::to_chars), which juce::JSON's
//     fixed 15 decimal places is not (read(write(p)) == p for every double);
//   - speed and layout: no tree of DynamicObjects per save, and a diff-friendly layout (one line per step, small
//     objects inline) chosen per container.
// Reading uses juce::JSON::parse (its number reader keeps 17 significant digits, so the shortest form reads back
// exactly).
#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace services::store::detail
{
class JsonWriter
{
public:
    enum class Layout { Lines, Inline }; // Lines: one member per line; Inline: all on one line
    explicit JsonWriter(std::string& out, bool compact = false) : out_(out), compact_(compact) {}

    void beginObject(Layout = Layout::Lines);
    void endObject();
    void beginArray(Layout = Layout::Inline);
    void endArray();
    void key(std::string_view);

    void value(bool);
    void value(int v) { value(static_cast<std::int64_t>(v)); }
    void value(std::int64_t);
    void value(double);
    void value(float); // shortest form that reads back to the same float (step locks)
    void value(std::string_view);
    void value(const char* s) { value(std::string_view(s)); }
    void null();
    void raw(std::string_view json); // a complete JSON value, written as is
    void var(const juce::var&);      // a parsed value (unknown keys)

private:
    void beforeValue();
    void newline(int depth);
    struct Frame
    {
        bool inl = false, first = true;
    };
    std::string& out_;
    bool compact_;
    std::array<Frame, 32> stack_ {};
    int depth_ = 0;
    bool afterKey_ = false;
};

void appendEscaped(std::string& out, std::string_view s);
// The canonical compact text of a parsed value (how unknown keys are kept in Extras).
std::string canonicalJson(const juce::var&);
} // namespace services::store::detail
