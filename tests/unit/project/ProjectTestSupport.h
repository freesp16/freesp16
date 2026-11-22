// Helpers of the project tests: a data folder with the session's layout, a command bus with a project, a sample
// library + pool + a swap-owning engine fake, generated samples. Everything here is generated: no device data.
#pragma once

#include "tests/unit/library/LibTestSupport.h"
#include "tests/unit/store/StoreTestSupport.h"

#include "core/commands/Command.h"
#include "core/commands/CommandBusAccess.h"
#include "core/model/ModelOps.h"
#include "services/library/SampleLibrary.h"
#include "services/library/SamplePool.h"
#include "services/library/TrackSampleSync.h"
#include "services/library/Workers.h"
#include "services/store/CodecRegistry.h"
#include "services/store/ProjectStoreSession.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace projtest
{
namespace m = core::model;
namespace st = services::store;

// A test edit: settings.tempo (undoable, never coalesces).
class SetTempo final : public core::Command
{
public:
    explicit SetTempo(double bpm) : bpm_(bpm) {}
    std::string_view label() const override { return "TEMPO"; }
    void apply(m::Project& p) override { p.settings.tempo = bpm_; }
    std::unique_ptr<core::Command> inverse(const m::Project& before) const override
    {
        return std::make_unique<SetTempo>(before.settings.tempo);
    }
    void collectChanges(core::ChangeSet& c) const override { c.add("settings.tempo"); }

private:
    double bpm_;
};

inline void setTempo(core::CommandBus& bus, double bpm)
{
    bus.execute(std::make_unique<SetTempo>(bpm));
    core::commands::flushNotifications(bus);
}

// A file's bytes and modification time, to prove a file was not touched.
struct FileStamp
{
    std::string bytes;
    juce::Time modified;
    friend bool operator== (const FileStamp&, const FileStamp&) = default;
};
inline FileStamp stamp(const juce::File& f) { return {storetest::readBytes(f), f.getLastModificationTime()}; }

// Every file of a folder (relative path → bytes), to prove a tree was not touched.
inline std::map<std::string, std::string> tree(const juce::File& folder)
{
    std::map<std::string, std::string> out;
    for (const auto& e : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
        out[e.getFile().getRelativePathFrom(folder).toStdString()] = storetest::readBytes(e.getFile());
    return out;
}

// The data folder of one test: <tmp>/data/{Projects,Backups,System}, a library root at <tmp>/Lib, workers, the bus
// with a default project, a pool resolving through the session, and an engine fake that owns installed swaps.
struct Env
{
    explicit Env(const char* name, st::SessionConfig extra = {})
        : Env(std::make_shared<libtest::TempDir>(name), std::move(extra))
    {
    }
    // Another "boot" on the same disk (the folder lives as long as any Env using it).
    explicit Env(std::shared_ptr<libtest::TempDir> disk, st::SessionConfig extra = {})
        : tmp(std::move(disk)), library(workers)
    {
        REQUIRE(library.setRoot(tmp->dir().getChildFile("Lib")).ok());
        st::SessionConfig c = std::move(extra);
        c.dataDir = tmp->dir().getChildFile("data");
        if (! c.sampleRoots)
            c.sampleRoots = [this] { return library.roots(); };
        if (! c.selection)
            c.selection = [this] { return selection; };
        c.useOsTrash = false; // never the user's real trash
        session = std::make_unique<st::ProjectSession>(bus, workers, &engine, &pool, c);
        sync = std::make_unique<services::library::TrackSampleSync>(pool, engine);
        session->onProjectReplaced = [this](const m::Project& p, const st::LoadReport& r)
        {
            ++replaced;
            lastReport = r;
            sync->syncAll(p);
        };
        core::commands::setBridge(bus, [this](const core::ChangeSet& cs, const m::Project& p) { sync->onChange(cs, p); });
    }
    ~Env()
    {
        workers.drain();
        sync.reset();
        session.reset();
        workers.drain();
    }

    st::ProjectStore& store() { return session->store(); }
    m::Project& project() { return core::commands::project(bus); }
    juce::File data() const { return tmp->dir().getChildFile("data"); }
    juce::File projects() const { return data().getChildFile("Projects"); }
    juce::File system() const { return data().getChildFile("System"); }
    juce::File samples() const { return library.root().getChildFile("Samples"); }

    // Runs an asynchronous store call to completion.
    core::Result<void> save(st::SaveKind kind, const juce::String& name, const juce::File& dir = {})
    {
        std::optional<core::Result<void>> r;
        st::Progress progress;
        store().save(kind, name, dir, progress, [&](core::Result<void> x) { r = std::move(x); });
        workers.drain();
        REQUIRE(r.has_value());
        return *r;
    }
    core::Result<void> open(const juce::File& bundle, st::LoadReport* report = nullptr)
    {
        std::optional<core::Result<void>> r;
        st::Progress progress;
        store().open(bundle, progress,
                     [&](core::Result<void> x, st::LoadReport rep)
                     {
                         r = std::move(x);
                         if (report != nullptr)
                             *report = std::move(rep);
                     });
        workers.drain();
        REQUIRE(r.has_value());
        return *r;
    }
    st::StartupResult startup()
    {
        std::optional<st::StartupResult> r;
        session->startup([&](const st::StartupResult& x) { r = x; });
        workers.drain();
        REQUIRE(r.has_value());
        return *r;
    }

    // A generated sample in the library and its pool record ($Lib path, content id).
    m::SampleRef sample(const std::string& file, double hz, std::int64_t frames = 2205)
    {
        const auto f = libtest::writeSample(samples().getChildFile(file), frames, hz);
        m::SampleRef ref;
        ref.id = st::contentId(f).value();
        ref.path = "$Lib/Samples/" + file;
        ref.displayName = file;
        return ref;
    }

    std::shared_ptr<libtest::TempDir> tmp;
    services::library::Workers workers {2};
    services::library::SampleLibrary library;
    core::CommandBus bus;
    services::library::SamplePool pool {workers, [this](const std::string& p) { return session->resolveSample(p); }};
    libtest::SwapEngine engine; // after the pool: releases its references before the pool frees the buffers
    std::unique_ptr<st::ProjectSession> session;
    std::unique_ptr<services::library::TrackSampleSync> sync;
    st::Selection selection {};
    int replaced = 0;
    st::LoadReport lastReport {};
};
} // namespace projtest
