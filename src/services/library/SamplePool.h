// The sample pool (LIB-030/031; data-model.md "SamplePool"): every sample of the open
// project decoded into RAM once, shared by every track that uses it, keyed by content (SampleBuffer::contentId, the
// XXH64 of the PCM data): the same file on 16 tracks — or two files with the same audio — is resident once.
//
// References (SampleBuffer::refs): the document holds one per track slot that uses a buffer (TrackSampleSync), the
// engine one per installed slot and per sounding voice. A buffer nobody references stays cached (so undo, preview →
// load and re-loads are instant) until the cache exceeds `cacheBytes` or memory is needed; collect() frees the oldest
// first. Freeing happens here, on the message thread — never on the audio thread, whose release() only decrements and
// bumps core::audio::zeroReleaseCount() (the retire path; SampleBufferStorage.h).
//
// Memory meter = residentBytes() / limitBytes(); default limits 4 GB on the host, 512 MB on the device (the
// unit's RAM size is not confirmed, research docs/hardware.md). A sample that does not fit is refused with a message.
// Reload rule (LIB-031): reloadIfChanged compares mtime + size, re-decodes and re-hashes on a change and, when the
// audio differs, swaps the new buffer into every slot that used the old one (onReplaced → TrackSampleSync).
//
// Message thread API; decoding runs on Workers.
#pragma once

#include "core/audio/SampleBuffer.h"
#include "core/model/Sample.h"
#include "core/util/Result.h"
#include "services/library/AudioFile.h"
#include "services/library/Workers.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace services::library
{
struct LoadInfo
{
    juce::File file;
    std::string contentId; // the model id, "sha256:<hex>" of the file (services::store::contentId), when known
    AudioFileInfo source{};
    std::vector<std::string> notes; // DecodeReport notes (mixdown, resampling, truncation, long-sample warning)
    bool resampled = false, mixedDown = false, truncated = false, longSample = false;
    std::optional<std::int64_t> loopStart, loopEnd; // the file's loop, engine-rate frames
    core::model::SampleMetadata metadata{};
    bool hasMetadata = false;
    bool cached = false; // served from RAM without decoding
};

struct Progress
{
    std::atomic<float> fraction{0.0f};
    std::atomic<bool> cancel{false};
};

// Min/max pyramid for waveform widgets: level 0 = kFinestBin frames per bin, each further level 4× coarser, down to one
// bin. Values are the min/max over all channels of the bin. The widgets read whole bins inside a column and the raw
// samples at its edges (ui::WaveOverview::ch), so the finest bin bounds their raw reads per column (< 2 bins); 64
// frames keeps that small on the device for 1 / 8 of a byte per frame (level 0 + the coarser levels ≈ 1 / 6).
struct Overview
{
    static constexpr std::int64_t kFinestBin = 64;
    struct Level
    {
        std::int64_t framesPerBin = 0;
        std::vector<float> min, max;
    };
    std::vector<Level> levels;
    static Overview build(const core::audio::SampleBuffer&);
};

class SamplePool
{
public:
    using Handle = core::audio::SampleBuffer const*;
    // Valid during the call; retain() it to keep it. LoadInfo describes the file.
    using Done = std::function<void(core::Result<Handle>, const LoadInfo&)>;
    // SampleRef::path → file (SampleLibrary::resolve; FMT-011 aliases).
    using Resolver = std::function<std::optional<juce::File>(const std::string& path)>;

    struct Config
    {
        double engineRate = 44100;
        std::int64_t limitBytes = defaultLimitBytes();
        std::int64_t cacheBytes = 64ll << 20; // unreferenced buffers kept for undo/preview
    };
    static std::int64_t defaultLimitBytes() noexcept;

    SamplePool(Workers&, Resolver, Config);
    SamplePool(Workers& w, Resolver r) : SamplePool(w, std::move(r), Config{}) {}
    ~SamplePool(); // frees every unreferenced buffer; a buffer still referenced is left alone (the engine owner
                   // releases its references before the pool goes)
    SamplePool(const SamplePool&) = delete;
    SamplePool& operator=(const SamplePool&) = delete;

    // Decodes (or finds) the sample `ref` names. ref.id empty → the file's content id is computed (LoadInfo::contentId,
    // for a new SampleRef); otherwise ref.id is trusted.
    void request(const core::model::SampleRef& ref, Done done, Progress* progress = nullptr);
    void requestFile(const juce::File& file, bool needContentId, Done done, Progress* progress = nullptr);

    std::int64_t residentBytes() const noexcept { return resident_.load(std::memory_order_relaxed); }
    std::int64_t limitBytes() const noexcept { return config_.limitBytes; }
    double engineRate() const noexcept { return config_.engineRate; }

    void reloadIfChanged(const core::model::SampleRef&);
    void reloadFileIfChanged(const juce::File&);
    void reloadAllIfChanged();
    // (old, new): a reload changed the audio of a file; every holder of `old` should move to `new`.
    std::function<void(Handle oldBuffer, Handle newBuffer)> onReplaced;

    const Overview& overview(Handle) const;
    Handle find(const juce::File&) const; // resident buffer of a file, nullptr if none
    std::optional<juce::File> fileOf(Handle) const;
    bool uses(const juce::File&) const; // referenced by a track or the engine (SampleLibrary::setInUse)

    // Frees unreferenced buffers above the cache budget (oldest first) and replaced ones; call once per frame (cheap
    // when nothing reached zero). Returns the number freed.
    std::size_t collect();
    std::size_t bufferCount() const noexcept { return buffers_.size(); }
    std::size_t pendingCount() const noexcept { return inFlight_.size(); }

private:
    struct Job;
    struct FileEntry
    {
        Handle buffer = nullptr;
        juce::Time modified;
        std::int64_t size = 0;
        LoadInfo info;
    };
    struct BufferEntry
    {
        std::size_t bytes = 0;
        std::uint64_t lastUse = 0;
        bool replaced = false;
        std::shared_ptr<Overview> overview;
    };
    struct Waiter
    {
        Done done;
        bool needContentId;
    };

    void start(const juce::File&, bool needContentId, Progress*, bool reload);
    void finish(const std::string& key, Job&);
    void deliver(Handle, const LoadInfo&, std::vector<Waiter>&);
    bool makeRoom(std::size_t bytes);
    void freeBuffer(Handle);
    std::int64_t evictableBytes() const;

    Workers& workers_;
    Resolver resolve_;
    Config config_;
    std::map<std::string, FileEntry> files_; // full path → entry
    std::map<Handle, BufferEntry> buffers_;
    std::map<std::uint64_t, Handle> byContent_;
    std::map<std::string, std::vector<Waiter>> inFlight_; // full path → waiters
    std::atomic<std::int64_t> resident_{0};
    std::uint64_t useClock_ = 0, lastZeroCount_ = ~std::uint64_t{0};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::library
