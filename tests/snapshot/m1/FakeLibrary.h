// A fake sample library with the BROWSE mockup's tree (browse.py ROWS): INTERNAL and USB "Samples" roots, the USB
// drums / loops folders and three loops; track 5 holds the Beat loop. Loads complete at once or when the test says.
#pragma once

#include "ui/screens/browse/BrowseSource.h"

#include <string>

namespace m1test
{
using ui::m1::BrowseEntry;

class FakeLibrary final : public ui::m1::BrowseSource
{
public:
    std::string assignedPath = "$Ext:USB/Samples/loops/Beat_120bpm_2bar.wav";
    bool deferLoads = false, playing = false;
    std::function<void(float)> pendingProgress;
    std::function<void(bool, std::string)> pendingDone;
    std::string lastPreview, loaded;
    int bigFolder = 0;

    std::vector<BrowseEntry> roots() override
    {
        return {{.name = "Samples", .path = "$Lib/Samples", .folder = true, .badge = "INTERNAL"},
                {.name = "Samples", .path = "$Ext:USB/Samples", .folder = true, .removable = true, .badge = "USB"}};
    }
    void list(const std::string& path, ui::m1::BrowseSort, std::function<void(std::vector<BrowseEntry>)> done) override
    {
        std::vector<BrowseEntry> out;
        if (path == "$Ext:USB/Samples")
            out = {{.name = "drums", .path = path + "/drums", .folder = true, .removable = true},
                   {.name = "loops", .path = path + "/loops", .folder = true, .removable = true}};
        else if (path == "$Ext:USB/Samples/loops")
            for (const char* n : {"Bass_120bpm_2bar.wav", "Beat_120bpm_2bar.wav", "Pad_A_120bpm_2bar.wav"})
                out.push_back({.name = n, .path = path + "/" + n, .removable = true});
        else if (path == "$Lib/Samples")
            for (int i = 0; i < bigFolder; ++i)
                out.push_back(
                    {.name = "Hit_" + std::to_string(i) + ".wav", .path = path + "/Hit_" + std::to_string(i) + ".wav"});
        done(std::move(out));
    }
    std::string freeSpace(const std::string&) override { return "1.0 GB FREE"; }
    const std::string& assigned(int t) override
    {
        static const std::string none;
        return t == 5 ? assignedPath : none;
    }
    void load(int, const std::string& path, std::function<void(float)> progress,
              std::function<void(bool, std::string)> done) override
    {
        loaded = path;
        if (deferLoads)
        {
            pendingProgress = std::move(progress);
            pendingDone = std::move(done);
            return;
        }
        progress(1);
        assignedPath = path;
        done(true, {});
    }
    void unload(int) override { assignedPath.clear(); }
    void preview(const std::string& path) override
    {
        lastPreview = path;
        playing = ! path.empty();
    }
    bool previewing() const override { return playing; }
};

} // namespace m1test
