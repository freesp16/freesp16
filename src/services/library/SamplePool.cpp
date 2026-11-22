#include "services/library/SamplePool.h"

#include "core/audio/SampleBufferStorage.h"
#include "services/analysis/PeakKernels.h"
#include "services/library/Decoder.h"
#include "services/library/SidecarStore.h"
#include "services/store/SampleRefs.h"

#include <algorithm>
#include <limits>

namespace services::library
{
using core::Result;
using core::audio::SampleBuffer;

struct SamplePool::Job
{
    // in
    juce::File file;
    bool needContentId = false, reload = false, idOnly = false;
    double engineRate = 44100;
    std::int64_t limitBytes = 0;
    juce::Time previousModified;
    std::int64_t previousSize = -1;
    Progress* progress = nullptr;
    // out
    bool unchanged = false;
    std::optional<core::Error> error;
    SampleBufferPtr buffer;
    std::shared_ptr<Overview> overview;
    LoadInfo info;
    juce::Time modified;
    std::int64_t size = 0;

    void run()
    {
        modified = file.getLastModificationTime();
        size = file.getSize();
        info.file = file;
        if (! file.existsAsFile())
        {
            error = core::Error{"Sample file not found", file.getFullPathName().toStdString()};
            return;
        }
        if (reload && modified == previousModified && size == previousSize)
        {
            unchanged = true;
            return;
        }
        if (needContentId)
        {
            auto id = services::store::contentId(file);
            if (! id)
            {
                error = id.error();
                return;
            }
            info.contentId = id.value();
        }
        if (idOnly)
            return;
        info.hasMetadata = SidecarStore().tryRead(file, info.metadata);
        DecodeReport report;
        DecodeOptions options;
        options.limitBytes = limitBytes;
        if (progress != nullptr)
        {
            options.cancel = &progress->cancel;
            options.progress = &progress->fraction;
        }
        auto decoded = Decoder::decode(file, engineRate, report, options);
        if (! decoded)
        {
            error = decoded.error();
            return;
        }
        info.source = report.source;
        info.notes = report.notes;
        info.resampled = report.resampled;
        info.mixedDown = report.mixedDown;
        info.truncated = report.truncated;
        info.longSample = report.longSample;
        info.loopStart = decoded.value().loopStart;
        info.loopEnd = decoded.value().loopEnd;
        buffer = std::move(decoded.value().buffer);
        overview = std::make_shared<Overview>(Overview::build(*buffer));
    }
};

Overview Overview::build(const SampleBuffer& b)
{
    Overview o;
    if (b.frames <= 0)
        return o;
    Level level;
    level.framesPerBin = Overview::kFinestBin;
    const std::int64_t fpb = level.framesPerBin, bins = (b.frames + fpb - 1) / fpb;
    level.min.assign(static_cast<std::size_t>(bins), 0.0f);
    level.max.assign(static_cast<std::size_t>(bins), 0.0f);
    for (std::int64_t bin = 0; bin < bins; ++bin)
    {
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        const std::int64_t begin = bin * fpb, end = std::min(b.frames, begin + fpb);
        for (int c = 0; c < b.channels; ++c)
            services::analysis::kernels::minMax(b.ch[c] + begin, static_cast<int>(end - begin), lo, hi);
        level.min[static_cast<std::size_t>(bin)] = lo;
        level.max[static_cast<std::size_t>(bin)] = hi;
    }
    o.levels.push_back(std::move(level));
    while (o.levels.back().min.size() > 1)
    {
        const Level& prev = o.levels.back();
        Level next;
        next.framesPerBin = prev.framesPerBin * 4;
        const std::size_t n = (prev.min.size() + 3) / 4;
        next.min.resize(n), next.max.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::size_t a = i * 4, e = std::min(prev.min.size(), a + 4);
            next.min[i] = *std::min_element(prev.min.begin() + static_cast<std::ptrdiff_t>(a),
                                            prev.min.begin() + static_cast<std::ptrdiff_t>(e));
            next.max[i] = *std::max_element(prev.max.begin() + static_cast<std::ptrdiff_t>(a),
                                            prev.max.begin() + static_cast<std::ptrdiff_t>(e));
        }
        o.levels.push_back(std::move(next));
    }
    return o;
}

std::int64_t SamplePool::defaultLimitBytes() noexcept
{
#if defined(__arm__)
    return 512ll << 20; // device
#else
    return 4ll << 30;
#endif
}

SamplePool::SamplePool(Workers& workers, Resolver resolver, Config config)
    : workers_(workers), resolve_(std::move(resolver)), config_(config)
{
}

SamplePool::~SamplePool()
{
    *alive_ = false;
    for (auto& [h, e] : buffers_)
        if (h->refs.load(std::memory_order_acquire) == 0)
            core::audio::freeSampleBuffer(h);
}

void SamplePool::request(const core::model::SampleRef& ref, Done done, Progress* progress)
{
    const auto file = resolve_ ? resolve_(ref.path) : std::optional<juce::File>{};
    if (! file)
    {
        LoadInfo info;
        done(Result<Handle>::failure("Sample file not found", ref.path), info);
        return;
    }
    const std::string id = ref.id;
    requestFile(
        *file, id.empty(),
        [id, done = std::move(done)](Result<Handle> r, const LoadInfo& info)
        {
            if (id.empty() || ! info.contentId.empty())
                return done(std::move(r), info);
            LoadInfo withId = info;
            withId.contentId = id;
            done(std::move(r), withId);
        },
        progress);
}

void SamplePool::requestFile(const juce::File& file, bool needContentId, Done done, Progress* progress)
{
    const std::string key = file.getFullPathName().toStdString();
    if (auto it = files_.find(key); it != files_.end())
    {
        if (needContentId && it->second.info.contentId.empty())
        {
            // Resident but never hashed: hash the file (worker), then deliver from RAM.
            auto job = std::make_shared<Job>();
            job->file = file, job->needContentId = true, job->idOnly = true;
            std::weak_ptr<bool> alive = alive_;
            workers_.run([job] { job->run(); },
                         [this, alive, job, key, done = std::move(done)]() mutable
                         {
                             if (alive.expired())
                                 return;
                             auto f = files_.find(key);
                             if (f == files_.end())
                                 return requestFile(job->file, true, std::move(done), nullptr);
                             if (job->error)
                                 return done(*job->error, f->second.info);
                             f->second.info.contentId = job->info.contentId;
                             std::vector<Waiter> w{{std::move(done), true}};
                             LoadInfo info = f->second.info;
                             info.cached = true;
                             deliver(f->second.buffer, info, w);
                         });
            return;
        }
        std::vector<Waiter> w{{std::move(done), needContentId}};
        LoadInfo info = it->second.info;
        info.cached = true;
        deliver(it->second.buffer, info, w);
        return;
    }
    if (auto it = inFlight_.find(key); it != inFlight_.end())
    {
        it->second.push_back({std::move(done), needContentId});
        return;
    }
    inFlight_[key].push_back({std::move(done), needContentId});
    start(file, needContentId, progress, false);
}

void SamplePool::start(const juce::File& file, bool needContentId, Progress* progress, bool reload)
{
    auto job = std::make_shared<Job>();
    const std::string key = file.getFullPathName().toStdString();
    job->file = file;
    job->needContentId = needContentId;
    job->reload = reload;
    job->engineRate = config_.engineRate;
    job->limitBytes = std::max<std::int64_t>(1, config_.limitBytes - residentBytes() + evictableBytes());
    job->progress = progress;
    if (auto it = files_.find(key); reload && it != files_.end())
    {
        job->previousModified = it->second.modified;
        job->previousSize = it->second.size;
    }
    std::weak_ptr<bool> alive = alive_;
    workers_.run([job] { job->run(); },
                 [this, alive, job, key]
                 {
                     if (! alive.expired())
                         finish(key, *job);
                 });
}

void SamplePool::finish(const std::string& key, Job& job)
{
    std::vector<Waiter> waiters;
    if (auto it = inFlight_.find(key); it != inFlight_.end())
    {
        waiters = std::move(it->second);
        inFlight_.erase(it);
    }
    if (job.unchanged)
        return;
    if (job.error)
    {
        for (auto& w : waiters)
            if (w.done)
                w.done(*job.error, job.info);
        return;
    }
    Handle buffer = nullptr;
    const std::uint64_t content = job.buffer->contentId;
    if (auto same = byContent_.find(content); same != byContent_.end())
        buffer = same->second; // the same audio is already resident (another file, or an unchanged reload)
    else
    {
        const std::size_t bytes = core::audio::sampleBufferBytes(job.buffer.get());
        if (! makeRoom(bytes))
        {
            const core::Error full{"Not enough memory for this sample", std::to_string(bytes) + " bytes needed"};
            for (auto& w : waiters)
                if (w.done)
                    w.done(full, job.info);
            return;
        }
        buffer = job.buffer.release();
        BufferEntry e;
        e.bytes = bytes;
        e.lastUse = ++useClock_;
        e.overview = job.overview;
        buffers_.emplace(buffer, std::move(e));
        byContent_[content] = buffer;
        resident_.fetch_add(static_cast<std::int64_t>(bytes), std::memory_order_relaxed);
    }

    Handle old = nullptr;
    FileEntry& f = files_[key];
    if (job.reload)
    {
        old = f.buffer;
        if (job.info.contentId.empty())
            job.info.contentId = f.info.contentId;
    }
    f.buffer = buffer;
    f.modified = job.modified;
    f.size = job.size;
    f.info = job.info;
    if (old != nullptr && old != buffer)
    {
        if (auto b = buffers_.find(old); b != buffers_.end())
            b->second.replaced = true;
        if (onReplaced)
        {
            retain(buffer); // keep both alive across the callback
            onReplaced(old, buffer);
            core::audio::release(buffer);
        }
    }
    deliver(buffer, f.info, waiters);
}

void SamplePool::deliver(Handle buffer, const LoadInfo& info, std::vector<Waiter>& waiters)
{
    if (auto b = buffers_.find(buffer); b != buffers_.end())
        b->second.lastUse = ++useClock_;
    core::audio::retain(buffer); // valid during the callbacks
    for (auto& w : waiters)
    {
        if (! w.done)
            continue;
        if (w.needContentId && info.contentId.empty())
        {
            requestFile(info.file, true, std::move(w.done), nullptr); // joined a load that did not hash: hash now
            continue;
        }
        w.done(buffer, info);
    }
    core::audio::release(buffer);
}

void SamplePool::reloadIfChanged(const core::model::SampleRef& ref)
{
    if (auto file = resolve_ ? resolve_(ref.path) : std::optional<juce::File>{})
        reloadFileIfChanged(*file);
}

void SamplePool::reloadFileIfChanged(const juce::File& file)
{
    const std::string key = file.getFullPathName().toStdString();
    auto it = files_.find(key);
    if (it == files_.end() || inFlight_.count(key) != 0)
        return;
    inFlight_[key];
    start(file, ! it->second.info.contentId.empty(), nullptr, true);
}

void SamplePool::reloadAllIfChanged()
{
    std::vector<juce::File> files;
    for (const auto& [key, e] : files_)
        files.push_back(e.info.file);
    for (const auto& f : files)
        reloadFileIfChanged(f);
}

const Overview& SamplePool::overview(Handle h) const
{
    static const Overview empty;
    const auto it = buffers_.find(h);
    return it != buffers_.end() && it->second.overview ? *it->second.overview : empty;
}

SamplePool::Handle SamplePool::find(const juce::File& file) const
{
    const auto it = files_.find(file.getFullPathName().toStdString());
    return it != files_.end() ? it->second.buffer : nullptr;
}

std::optional<juce::File> SamplePool::fileOf(Handle h) const
{
    for (const auto& [key, e] : files_)
        if (e.buffer == h)
            return e.info.file;
    return std::nullopt;
}

bool SamplePool::uses(const juce::File& file) const
{
    const Handle h = find(file);
    return h != nullptr && h->refs.load(std::memory_order_acquire) > 0;
}

std::int64_t SamplePool::evictableBytes() const
{
    std::int64_t total = 0;
    for (const auto& [h, e] : buffers_)
        if (h->refs.load(std::memory_order_acquire) == 0)
            total += static_cast<std::int64_t>(e.bytes);
    return total;
}

void SamplePool::freeBuffer(Handle h)
{
    const auto it = buffers_.find(h);
    if (it == buffers_.end())
        return;
    resident_.fetch_sub(static_cast<std::int64_t>(it->second.bytes), std::memory_order_relaxed);
    buffers_.erase(it);
    if (auto c = byContent_.find(h->contentId); c != byContent_.end() && c->second == h)
        byContent_.erase(c);
    for (auto f = files_.begin(); f != files_.end();)
        f = f->second.buffer == h ? files_.erase(f) : std::next(f);
    core::audio::freeSampleBuffer(h);
}

bool SamplePool::makeRoom(std::size_t bytes)
{
    while (residentBytes() + static_cast<std::int64_t>(bytes) > config_.limitBytes)
    {
        Handle oldest = nullptr;
        std::uint64_t when = ~std::uint64_t{0};
        for (const auto& [h, e] : buffers_)
            if (h->refs.load(std::memory_order_acquire) == 0 && e.lastUse < when)
                oldest = h, when = e.lastUse;
        if (oldest == nullptr)
            return false;
        freeBuffer(oldest);
    }
    return true;
}

std::size_t SamplePool::collect()
{
    const std::uint64_t zero = core::audio::zeroReleaseCount();
    if (zero == lastZeroCount_)
        return 0;
    lastZeroCount_ = zero;
    std::size_t freed = 0;
    std::vector<Handle> replaced;
    std::int64_t cached = 0;
    for (const auto& [h, e] : buffers_)
        if (h->refs.load(std::memory_order_acquire) == 0)
        {
            if (e.replaced)
                replaced.push_back(h);
            else
                cached += static_cast<std::int64_t>(e.bytes);
        }
    for (Handle h : replaced)
        freeBuffer(h), ++freed;
    while (cached > config_.cacheBytes)
    {
        Handle oldest = nullptr;
        std::uint64_t when = ~std::uint64_t{0};
        for (const auto& [h, e] : buffers_)
            if (h->refs.load(std::memory_order_acquire) == 0 && e.lastUse < when)
                oldest = h, when = e.lastUse;
        if (oldest == nullptr)
            break;
        cached -= static_cast<std::int64_t>(buffers_[oldest].bytes);
        freeBuffer(oldest), ++freed;
    }
    return freed;
}
} // namespace services::library
