#include "services/library/TrackLoader.h"

#include "core/commands/CommandGroup.h"
#include "services/analysis/Analysis.h"
#include "services/library/AudioFile.h"

#include <algorithm>
#include <vector>

namespace services::library
{
namespace cmd = core::commands::library;

TrackLoader::TrackLoader(SampleLibrary& library, SamplePool& pool, core::CommandBus& bus)
    : library_(library), pool_(pool), bus_(bus)
{
}

TrackLoader::~TrackLoader()
{
    *alive_ = false;
}

core::model::SampleRef TrackLoader::refFor(const juce::File& file, const std::string& contentId) const
{
    core::model::SampleRef ref;
    ref.id = contentId;
    const auto alias = library_.aliasPathOf(file);
    ref.path = (alias ? *alias : file.getFullPathName()).toStdString();
    ref.displayName = file.getFileNameWithoutExtension().toStdString();
    return ref;
}

cmd::LoadDefaults TrackLoader::defaultsFor(const core::audio::SampleBuffer& b, const LoadInfo& info)
{
    std::optional<double> bpm, bars;
    if (info.hasMetadata && info.metadata.bpm > 0)
        bpm = info.metadata.bpm;
    if (info.hasMetadata && info.metadata.bars > 0)
        bars = info.metadata.bars;
    auto d = cmd::classify(b.frames, b.sampleRate, bpm, bars, info.loopStart.has_value());
    // REN-024: the render analysis' classifier decides one-shot / loop (sidecar bars, the whole-bar length
    // rule at 70–180 BPM, a per-sample override); a sidecar BPM still marks a loop (the tempo is known, PLY-034).
    const auto verdict = services::analysis::classify(b, info.hasMetadata ? info.metadata : core::model::SampleMetadata {});
    d.loop = verdict == services::analysis::SampleClass::Loop || bpm.has_value();
    return d;
}

void TrackLoader::load(int scene, int track, const juce::File& file, Done done)
{
    std::weak_ptr<bool> alive = alive_;
    pool_.requestFile(
        file, true,
        [this, alive, scene, track, file, done](core::Result<SamplePool::Handle> r, const LoadInfo& info)
        {
            if (alive.expired())
                return;
            if (! r)
            {
                if (done)
                    done(r.error(), info);
                return;
            }
            // The pool keeps the buffer cached until TrackSampleSync takes the document's reference (same message
            // turn).
            bus_.execute(cmd::loadSample(scene, track, refFor(file, info.contentId), defaultsFor(*r.value(), info)));
            if (done)
                done(core::Result<void>{}, info);
        });
}

bool TrackLoader::onPadPressed(int scene, int track, Done done)
{
    if (! armed())
        return false;
    const juce::File f = armed_;
    disarm();
    load(scene, track, f, std::move(done));
    return true;
}

void TrackLoader::loadFolder(int scene, const juce::File& folder, std::function<void(core::Result<void>)> done)
{
    std::vector<juce::File> files;
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no))
        if (isAudioFileName(f.getFileName()))
            files.push_back(f);
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareNatural(b.getFileName(), false) < 0; });
    if (files.size() > 16)
        files.resize(16);
    if (files.empty())
    {
        if (done)
            done(core::Result<void>::failure("No audio files in this folder"));
        return;
    }
    struct State
    {
        std::vector<core::model::SampleRef> refs;
        std::vector<cmd::LoadDefaults> defaults;
        std::vector<bool> ok;
        std::size_t left;
    };
    auto st = std::make_shared<State>();
    st->refs.resize(files.size());
    st->defaults.resize(files.size());
    st->ok.assign(files.size(), false);
    st->left = files.size();
    std::weak_ptr<bool> alive = alive_;
    for (std::size_t i = 0; i < files.size(); ++i)
        pool_.requestFile(
            files[i], true,
            [this, alive, st, i, scene, file = files[i], done](core::Result<SamplePool::Handle> r, const LoadInfo& info)
            {
                if (alive.expired())
                    return;
                if (r)
                {
                    st->refs[i] = refFor(file, info.contentId);
                    st->defaults[i] = defaultsFor(*r.value(), info);
                    st->ok[i] = true;
                }
                if (--st->left > 0)
                    return;
                // Files that failed keep their track as it was: load the ones that decoded, in track order.
                const bool any = std::find(st->ok.begin(), st->ok.end(), true) != st->ok.end();
                if (! any)
                {
                    if (done)
                        done(core::Result<void>::failure("None of the files could be loaded"));
                    return;
                }
                // A failed file leaves a gap: build per-track loads only for the decoded ones.
                auto group = std::make_unique<core::CommandGroup>("SAMPLE");
                for (std::size_t k = 0; k < st->refs.size(); ++k)
                    if (st->ok[k])
                        group->add(cmd::loadSample(scene, static_cast<int>(k), st->refs[k], st->defaults[k]));
                bus_.execute(std::move(group));
                if (done)
                    done(core::Result<void>{});
            });
}
} // namespace services::library
