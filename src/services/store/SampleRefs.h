// Sample references of the native format (FMT-011, FMT-012, PRJ-014, PRJ-040/041; data-model.md "SamplePool").
//
// A pool entry's id is the content hash of the sample file, "sha256:<64 lower-case hex digits>" (contentId). Its
// location is one string in the model (SampleRef::path) and two fields in the file (`location` + `path`):
//     "$Lib/<rel>"          the user's sample library            → { "location": "$Lib",        "path": "<rel>" }
//     "$Bundle/<rel>"       the bundle's samples/ folder         → { "location": "$Bundle",     "path": "<rel>" }
//     "$Ext:<name>/<rel>"   an external volume known by name     → { "location": "$Ext:<name>", "path": "<rel>" }
//     anything else         an absolute path, kept verbatim      → { "location": "absolute",    "path": "<as is>" }
// splitLocation / joinLocation are exact inverses for every string (a string that does not split cleanly is
// "absolute").
//
// Resolution never trusts a relative path: "..", empty components and absolute <rel> are refused, so a project can
// never make the app read (or Collect & Save write) outside its roots. Saves never move or delete samples (PRJ-014).
// Worker threads only.
#pragma once

#include "core/model/Sample.h"
#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace services::store
{
// SHA-256 (FIPS 180-4), streaming. Portable scalar code: the A9 has no SHA instructions (≈ 25–40 MB/s expected there,
// unmeasured; hashing runs once per imported sample on a worker).
class Sha256
{
public:
    Sha256() noexcept { reset(); }
    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    std::array<std::uint8_t, 32> finish() noexcept; // resets afterwards
    static std::string hex(const std::array<std::uint8_t, 32>& digest);

private:
    void block(const std::uint8_t* p) noexcept;
    std::array<std::uint32_t, 8> h_ {};
    std::array<std::uint8_t, 64> buffer_ {};
    std::uint64_t length_ = 0;
    std::size_t used_ = 0;
};

inline constexpr std::string_view kContentIdPrefix = "sha256:";

// "sha256:<hex>" of a file's bytes, or an error naming the file.
core::Result<std::string> contentId(const juce::File& file);
std::string contentIdOf(const void* data, std::size_t size);
bool isContentId(std::string_view id) noexcept;

enum class LocationKind : std::uint8_t { Library, Bundle, External, Absolute };

struct SampleLocation
{
    LocationKind kind = LocationKind::Absolute;
    std::string volume; // External only: the volume name
    std::string path;   // relative to the root (Absolute: the whole string)
    friend bool operator== (const SampleLocation&, const SampleLocation&) = default;
};

SampleLocation splitLocation(std::string_view modelPath);
std::string joinLocation(const SampleLocation&);
// The file form of a location: "$Lib", "$Bundle", "$Ext:<name>", "absolute"; parseLocationTag is its inverse.
std::string locationTag(const SampleLocation&);
std::optional<SampleLocation> parseLocationTag(std::string_view tag, std::string path);

// Where the roots are on this machine (platform paths; the library and the project store fill them).
struct SampleRoots
{
    juce::File library;                          // $Lib
    juce::File bundle;                           // the project bundle ($Bundle = bundle/samples)
    std::map<std::string, juce::File> externals; // $Ext:<name>
};

// The file a location names, or nothing (unknown root, unsafe relative path).
std::optional<juce::File> resolveLocation(const SampleLocation&, const SampleRoots&);
// Sets SampleRef::missing on every pool entry (PRJ-040/041: a missing file is a pool state, never an error) and
// returns the number of missing entries.
int resolvePool(core::model::SamplePool&, const SampleRoots&);

// A file name that is safe on FAT32 and in every root: no path separators, no characters FAT refuses, no leading or
// trailing dots/spaces, at most 64 characters, never empty.
std::string safeFileName(std::string_view name);
} // namespace services::store
