// Fuzz entry points (NFR-011): one function per input kind, shared by the
// deterministic mutation corpus in ctest (test_fuzz.cpp, ASan/UBSan in the host preset) and by libFuzzer builds
// (fuzz_main.cpp, host clang; inert unless APP_LIBFUZZER_TARGET is defined). Each returns 0 and must never crash,
// hang or allocate without bound for any input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace projfuzz
{
// Which bundle file the bytes replace; the others are a default project's files.
enum class BundleFile : int { Project, Meta, Scene };
// The native-format pipeline from memory: parse → migrate → decode → validate → re-encode.
int bundleFile(BundleFile, const std::uint8_t* data, std::size_t size);
int projectJson(const std::uint8_t* data, std::size_t size); // = bundleFile(Project, …)
int autosave(const std::uint8_t* data, std::size_t size);    // Autosave::decodeText
int audio(const std::uint8_t* data, std::size_t size);       // WAV / AIFF probe + decode
int sidecar(const std::uint8_t* data, std::size_t size);     // .wav.dat + .wav.json sidecars

// The default project's encoded files (the base the corpus mutates).
const std::string& defaultFile(BundleFile);
} // namespace projfuzz
