// Opaque round-trip data of one model entity: values a reader found but the model has
// no field for — unknown JSON keys of a newer native file (kept and written back), reference-format
// nodes the importer must keep (slice grids OQ-04, the 8-byte arrangement record OQ-02, unknown enum strings OQ-10).
// Every entity carries one `extras` member; Step cells do not (they are 8-byte matrix cells), their opaque data lives
// in the owning Pattern's extras under "step[s].track[t].<key>".
//
// Plain value type, message thread only (never read by the engine). The codec that wrote an entry owns its format.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace core::model
{
struct Extras
{
    enum class Encoding : std::uint8_t
    {
        Json,  // a JSON value as text (native codec: an unknown key's value)
        Text,  // a plain string (e.g. an unknown enum string)
        Base64 // bytes (e.g. a reference-format blob)
    };
    struct Entry
    {
        std::string key;   // codec-qualified: "native.<key>", "sp16.<node>"
        Encoding encoding = Encoding::Json;
        std::string value;
        friend bool operator== (const Entry&, const Entry&) = default;
    };
    std::vector<Entry> entries;

    bool empty() const noexcept { return entries.empty(); }
    const Entry* find(std::string_view key) const noexcept
    {
        for (const auto& e : entries)
            if (e.key == key)
                return &e;
        return nullptr;
    }
    void set(std::string key, std::string value, Encoding encoding = Encoding::Json)
    {
        for (auto& e : entries)
            if (e.key == key)
            {
                e.value = std::move(value);
                e.encoding = encoding;
                return;
            }
        entries.push_back({std::move(key), encoding, std::move(value)});
    }
    friend bool operator== (const Extras&, const Extras&) = default;
};
} // namespace core::model
