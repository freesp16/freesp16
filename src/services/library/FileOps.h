// Library file operations (LIB-015; research docs/storage.md §6): copy, move, rename, trash, new folder.
//
//   copy   read → <dest>.tmp-<random> → fsync the file → rename to <dest> → fsync the folder
//   move   same volume: rename + fsync both folders; across volumes (EXDEV): copy as above, and only once the copy is
//          fsync'ed and renamed into place is the source deleted (fixes the reference's copy-then-delete without
//          sync, storage.md §6)
//   trash  move into <library>/System/trash/ (the device has no OS trash; the host uses the same folder so a
//          test never touches the user's Trash), a unique name on collision
// Names are kept FAT-safe (services::store::safeFileName); a name that already exists gets " 2", " 3", ….
// Worker threads only (blocking I/O).
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <string_view>

namespace services::library
{
class FileOps
{
public:
    // Observes the durable steps ("write", "fsync", "rename", "fsync-dir", "delete-source"), in order (tests).
    using Observer = std::function<void(std::string_view step, const juce::File&)>;

    explicit FileOps(juce::File trashFolder) : trash_(std::move(trashFolder)) {}

    core::Result<juce::File> copy(const juce::File& source, const juce::File& destFolder) const;
    core::Result<juce::File> move(const juce::File& source, const juce::File& destFolder) const;
    core::Result<juce::File> rename(const juce::File& source, const juce::String& newName) const;
    core::Result<juce::File> trash(const juce::File& source) const;
    core::Result<juce::File> newFolder(const juce::File& parent, const juce::String& name) const;

    void setObserver(Observer o) { observer_ = std::move(o); }
    void setForceCrossVolume(bool on) noexcept { forceCrossVolume_ = on; } // tests: two temp folders act as volumes

    // A non-existing sibling name in `folder`: "name.ext", then "name 2.ext", …
    static juce::File uniqueChild(const juce::File& folder, const juce::String& fileName);

private:
    core::Result<juce::File> copyTo(const juce::File& source, const juce::File& target) const;
    core::Result<juce::File> moveTo(const juce::File& source, const juce::File& target) const;
    void step(std::string_view s, const juce::File& f) const
    {
        if (observer_)
            observer_(s, f);
    }

    juce::File trash_;
    Observer observer_;
    bool forceCrossVolume_ = false;
};

bool fsyncPath(const juce::File& fileOrFolder); // true on success (a folder fsync may fail on FAT: tolerated)
} // namespace services::library
