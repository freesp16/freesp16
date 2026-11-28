#include "services/plugin/ClapHost.h"

#include "core/commands/Command.h"
#include "core/model/Project.h"

#include <clap/clap.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits>

namespace services::plugin
{
namespace
{
// Idle threshold before putting a silent CLAP plugin to sleep (~5 s at 44.1 kHz / 64 frames).
constexpr int kIdleSilenceBlocks = 3450;
constexpr float kSilencePeak = 1e-6f;
// Characters outside the base64 alphabet distinguish parameter-only patches from opaque CLAP state.
constexpr std::string_view kParamsPrefix = "params-v1:";

std::string resolveClapBinary(const juce::File& item)
{
    if (item.existsAsFile())
        return item.getFullPathName().toStdString();
    // macOS .clap bundle layout: Foo.clap/Contents/MacOS/<binary>
    if (item.isDirectory())
    {
        const auto macDir = item.getChildFile("Contents").getChildFile("MacOS");
        if (macDir.isDirectory())
        {
            const auto candidate = macDir.getChildFile(item.getFileNameWithoutExtension());
            if (candidate.existsAsFile())
                return candidate.getFullPathName().toStdString();
            for (const auto& f : macDir.findChildFiles(juce::File::findFiles, false))
                if (f.existsAsFile())
                    return f.getFullPathName().toStdString();
        }
    }
    return {};
}

// Host extension callbacks (minimal, RT-safe)
void hostParamsRescan(const clap_host_t*, clap_param_rescan_flags) noexcept {}
void hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) noexcept {}
void hostLogLog(const clap_host_t*, clap_log_severity, const char*) noexcept {}

const clap_host_log_t kHostLogExt = {&hostLogLog};

// Fixed-capacity CLAP input event list for one audio block (zero heap alloc).
struct BlockEventList
{
    static constexpr std::size_t kMax = 128;
    union Storage
    {
        clap_event_header_t header;
        clap_event_note_t note;
        clap_event_param_value_t param;
    };
    std::array<Storage, kMax> items{};
    std::uint32_t count = 0;
    clap_input_events_t iface{};

    BlockEventList() noexcept
    {
        iface.ctx = this;
        iface.size = &BlockEventList::sizeFn;
        iface.get = &BlockEventList::getFn;
    }

    static std::uint32_t sizeFn(const clap_input_events_t* list) noexcept
    {
        return static_cast<const BlockEventList*>(list->ctx)->count;
    }
    static const clap_event_header_t* getFn(const clap_input_events_t* list, std::uint32_t idx) noexcept
    {
        const auto* self = static_cast<const BlockEventList*>(list->ctx);
        return idx < self->count ? &self->items[idx].header : nullptr;
    }

    void pushNote(std::uint16_t type, std::uint32_t time, int key, double velocity) noexcept
    {
        if (count >= kMax)
            return;
        auto& n = items[count++].note;
        n = {};
        n.header.size = sizeof(clap_event_note_t);
        n.header.time = time;
        n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        n.header.type = type;
        n.header.flags = 0;
        n.note_id = -1;
        n.port_index = 0;
        n.channel = 0;
        n.key = static_cast<std::int16_t>(key);
        n.velocity = velocity;
    }

    void pushParam(std::uint32_t time, clap_id id, double value) noexcept
    {
        if (count >= kMax)
            return;
        auto& p = items[count++].param;
        p = {};
        p.header.size = sizeof(clap_event_param_value_t);
        p.header.time = time;
        p.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        p.header.type = CLAP_EVENT_PARAM_VALUE;
        p.header.flags = 0;
        p.param_id = id;
        p.cookie = nullptr;
        p.note_id = -1;
        p.port_index = -1;
        p.channel = -1;
        p.key = -1;
        p.value = value;
    }
};

struct DiscardOutEvents
{
    clap_output_events_t iface{};
    DiscardOutEvents() noexcept
    {
        iface.ctx = this;
        iface.try_push = [](const clap_output_events_t*, const clap_event_header_t*) noexcept -> bool { return true; };
    }
};

void copyTrunc(char* dst, std::size_t dstSize, std::string_view src) noexcept
{
    if (dstSize == 0)
        return;
    const std::size_t n = std::min(dstSize - 1, src.size());
    if (n > 0)
        std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

bool hasFeature(const clap_plugin_descriptor_t& desc, const char* feature) noexcept
{
    for (auto p = desc.features; p && *p; ++p)
        if (std::strcmp(*p, feature) == 0)
            return true;
    return false;
}

template <class Project> auto& effectAt(Project& project, int scene, int slot)
{
    auto& sc = project.scenes[scene];
    if (slot < 2 * engine::kTracks)
        return sc.tracks[slot / 2].insertFx[slot % 2];
    return slot == 2 * engine::kTracks ? sc.sendFx : sc.masterFx;
}

bool takeIndex(std::string_view& path, std::string_view prefix, int& index) noexcept
{
    if (! path.starts_with(prefix))
        return false;
    auto text = path.substr(prefix.size());
    const auto r = std::from_chars(text.data(), text.data() + text.size(), index);
    if (r.ec != std::errc{} || r.ptr == text.data() + text.size() || *r.ptr != ']' || index < 0 || index >= 16)
        return false;
    path = text.substr(static_cast<std::size_t>(r.ptr - text.data()) + 1);
    return true;
}
} // namespace

std::vector<CatalogueEntry> scanPluginDirectory(const std::string& pluginsDir, double sampleRate)
{
    std::vector<CatalogueEntry> out;
    if (pluginsDir.empty())
        return out;
    const juce::File root(pluginsDir);
    if (! root.isDirectory())
        return out;

    // Collect *.clap files and *.clap bundle directories (avoid descending into macOS .clap bundles).
    std::vector<juce::File> candidates;
    std::vector<juce::File> stack{root};
    while (! stack.empty())
    {
        const juce::File dir = stack.back();
        stack.pop_back();
        for (const auto& entry : dir.findChildFiles(juce::File::findFilesAndDirectories, false))
        {
            if (entry.hasFileExtension(".clap"))
                candidates.push_back(entry);
            else if (entry.isDirectory())
                stack.push_back(entry);
        }
    }

    for (const auto& item : candidates)
    {
        const std::string bin = resolveClapBinary(item);
        if (bin.empty())
            continue;
        void* handle = dlopen(bin.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (handle == nullptr)
            continue;
        const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
        if (entry == nullptr || ! clap_version_is_compatible(entry->clap_version) || entry->get_factory == nullptr ||
            entry->init == nullptr || ! entry->init(bin.c_str()))
        {
            dlclose(handle);
            continue;
        }
        const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (factory != nullptr && factory->get_plugin_count != nullptr && factory->get_plugin_descriptor != nullptr)
        {
            const std::uint32_t count = factory->get_plugin_count(factory);
            const std::string rel = item.getRelativePathFrom(root).replaceCharacter('\\', '/').toStdString();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, i);
                if (desc == nullptr || desc->id == nullptr)
                    continue;
                CatalogueEntry ce;
                ce.id = desc->id;
                ce.name = desc->name != nullptr && desc->name[0] != '\0' ? desc->name : desc->id;
                ce.vendor = desc->vendor != nullptr ? desc->vendor : "";
                ce.path = rel;
                ce.absPath = bin;
                const bool fx = hasFeature(*desc, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);
                ce.trackCompatible = ! fx || hasFeature(*desc, CLAP_PLUGIN_FEATURE_INSTRUMENT);
                ce.effectCompatible = fx;
                out.push_back(std::move(ce));
            }
        }
        if (entry->deinit != nullptr)
            entry->deinit();
        dlclose(handle);
    }

    // Probe after releasing the scan handle, using the same layout and activation checks as mounting.
    for (auto& candidate : out)
        if (candidate.effectCompatible)
        {
            std::string error;
            auto* node = ClapAudioNode::create(candidate.absPath, candidate.id, sampleRate, error, true);
            candidate.effectCompatible = node != nullptr;
            if (node)
                node->release();
        }

    std::sort(out.begin(), out.end(), [](const CatalogueEntry& a, const CatalogueEntry& b)
              { return a.name != b.name ? a.name < b.name : a.id < b.id; });
    return out;
}

// ---- ClapAudioNode --------------------------------------------------------------------------------------------------

const void* ClapAudioNode::hostGetExtension(const clap_host* host, const char* extId) noexcept
{
    if (extId == nullptr || host == nullptr)
        return nullptr;
    if (std::strcmp(extId, CLAP_EXT_LOG) == 0)
        return &kHostLogExt;
    if (std::strcmp(extId, CLAP_EXT_PARAMS) == 0)
    {
        static const clap_host_params_t kHostParams = {
            &hostParamsRescan,
            &hostParamsClear,
            [](const clap_host_t* h) noexcept
            {
                if (h != nullptr && h->host_data != nullptr)
                    static_cast<ClapAudioNode*>(h->host_data)->flushRequested_.store(true, std::memory_order_release);
            },
        };
        return &kHostParams;
    }
    return nullptr;
}

void ClapAudioNode::hostRequestRestart(const clap_host*) noexcept {}
void ClapAudioNode::hostRequestProcess(const clap_host* host) noexcept
{
    if (host != nullptr && host->host_data != nullptr)
        static_cast<ClapAudioNode*>(host->host_data)->wakeAudio_.store(true, std::memory_order_release);
}
void ClapAudioNode::hostRequestCallback(const clap_host* host) noexcept
{
    if (host != nullptr && host->host_data != nullptr)
        static_cast<ClapAudioNode*>(host->host_data)->callbackRequested_.store(true, std::memory_order_release);
}

ClapAudioNode* ClapAudioNode::create(const std::string& absPath, const std::string& pluginId, double sampleRate,
                                     std::string& errorOut, bool effect, bool stateOnly, std::string_view initialParams)
{
    const std::string bin = resolveClapBinary(juce::File(absPath));
    const std::string target = bin.empty() ? absPath : bin;
    void* handle = dlopen(target.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr)
    {
        const char* err = dlerror();
        errorOut = std::string("dlopen failed: ") + (err != nullptr ? err : "unknown error");
        return nullptr;
    }
    const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (entry == nullptr || ! clap_version_is_compatible(entry->clap_version) || entry->get_factory == nullptr ||
        entry->init == nullptr || ! entry->init(target.c_str()))
    {
        dlclose(handle);
        errorOut = "invalid or incompatible clap_entry";
        return nullptr;
    }
    const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr || factory->create_plugin == nullptr || factory->get_plugin_count == nullptr ||
        factory->get_plugin_descriptor == nullptr)
    {
        if (entry->deinit != nullptr)
            entry->deinit();
        dlclose(handle);
        errorOut = "missing CLAP plugin factory";
        return nullptr;
    }

    const clap_plugin_descriptor_t* chosenDesc = nullptr;
    const std::uint32_t count = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto* d = factory->get_plugin_descriptor(factory, i);
        if (d == nullptr || d->id == nullptr)
            continue;
        if (pluginId.empty() || pluginId == d->id)
        {
            chosenDesc = d;
            break;
        }
    }
    if (chosenDesc == nullptr)
    {
        if (entry->deinit != nullptr)
            entry->deinit();
        dlclose(handle);
        errorOut = "plugin id not found in bundle";
        return nullptr;
    }

    auto* node = new ClapAudioNode();
    node->dlHandle_ = handle;
    node->entry_ = entry;
    node->absPath_ = target;
    node->id_ = chosenDesc->id;
    node->name_ = chosenDesc->name != nullptr && chosenDesc->name[0] != '\0' ? chosenDesc->name : chosenDesc->id;
    node->vendor_ = chosenDesc->vendor != nullptr ? chosenDesc->vendor : "";
    node->sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    node->hostStorage_ = new clap_host_t{};
    node->hostStorage_->clap_version = CLAP_VERSION_INIT;
    node->hostStorage_->host_data = node;
    node->hostStorage_->name = "Sixteen CLAP Host";
    node->hostStorage_->vendor = "Sixteen";
    node->hostStorage_->url = "";
    node->hostStorage_->version = "1.0.0";
    node->hostStorage_->get_extension = &ClapAudioNode::hostGetExtension;
    node->hostStorage_->request_restart = &ClapAudioNode::hostRequestRestart;
    node->hostStorage_->request_process = &ClapAudioNode::hostRequestProcess;
    node->hostStorage_->request_callback = &ClapAudioNode::hostRequestCallback;

    node->plugin_ = factory->create_plugin(factory, node->hostStorage_, chosenDesc->id);
    if (node->plugin_ == nullptr || node->plugin_->init == nullptr || ! node->plugin_->init(node->plugin_))
    {
        errorOut = "clap plugin init failed";
        delete node;
        return nullptr;
    }

    if (node->plugin_->get_extension != nullptr)
    {
        node->paramsExt_ =
            static_cast<const clap_plugin_params_t*>(node->plugin_->get_extension(node->plugin_, CLAP_EXT_PARAMS));
        node->stateExt_ =
            static_cast<const clap_plugin_state_t*>(node->plugin_->get_extension(node->plugin_, CLAP_EXT_STATE));
        auto getPresetLoad = [&](const char* ext)
        { return static_cast<const clap_plugin_preset_load_t*>(node->plugin_->get_extension(node->plugin_, ext)); };
        node->presetLoad_ = getPresetLoad(CLAP_EXT_PRESET_LOAD);
        if (! node->presetLoad_)
            node->presetLoad_ = getPresetLoad(CLAP_EXT_PRESET_LOAD_COMPAT);
        node->screenExt_ = static_cast<const sixteen_plugin_screen_t*>(
            node->plugin_->get_extension(node->plugin_, SIXTEEN_EXT_SCREEN));
        if (node->screenExt_ && ! node->screenExt_->render)
            node->screenExt_ = nullptr;
    }

    const auto* ports = node->plugin_->get_extension
                            ? static_cast<const clap_plugin_audio_ports_t*>(
                                  node->plugin_->get_extension(node->plugin_, CLAP_EXT_AUDIO_PORTS))
                            : nullptr;
    // Older track instruments without port metadata use the original silent stereo input.
    if (effect || ports)
    {
        clap_audio_port_info_t input{}, output{};
        const bool validPorts = ports && ports->count && ports->get;
        const auto inputCount = validPorts ? ports->count(node->plugin_, true) : 0u;
        if (! clap_version_is_compatible(chosenDesc->clap_version) || ! validPorts ||
            (effect && (! hasFeature(*chosenDesc, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) || inputCount != 1)) ||
            inputCount > 1 || ports->count(node->plugin_, false) != 1 ||
            (inputCount == 1 &&
             (! ports->get(node->plugin_, 0, true, &input) || input.channel_count < 1 || input.channel_count > 2)) ||
            ! ports->get(node->plugin_, 0, false, &output) || output.channel_count < 1 || output.channel_count > 2)
        {
            errorOut = effect ? "effect needs one mono or stereo input and output"
                              : "instrument needs one mono or stereo output and at most one mono or stereo input";
            delete node;
            return nullptr;
        }
        node->inputChannels_ = input.channel_count;
        node->outputChannels_ = output.channel_count;
    }
    if (! initialParams.empty())
    {
        node->refreshParamMeta();
        if (! node->loadStateBase64(initialParams))
        {
            errorOut = "Plugin rejected the saved parameters";
            delete node;
            return nullptr;
        }
    }
    if (stateOnly)
    {
        node->refreshParamMeta();
        return node;
    }
    if (node->plugin_->activate != nullptr)
    {
        if (! node->plugin_->activate(node->plugin_, node->sampleRate_, engine::kBlockFrames, engine::kBlockFrames))
        {
            errorOut = "clap plugin activate failed";
            delete node;
            return nullptr;
        }
        node->activated_ = true;
    }
    if (node->activated_ && node->plugin_->start_processing != nullptr)
        node->processing_ = node->plugin_->start_processing(node->plugin_);

    if (! node->processing_ || ! node->plugin_->process)
    {
        errorOut = "plugin could not start processing";
        delete node;
        return nullptr;
    }
    if (effect)
        if (const auto* latency = static_cast<const clap_plugin_latency_t*>(
                node->plugin_->get_extension(node->plugin_, CLAP_EXT_LATENCY));
            latency && latency->get)
            node->latencySamples_ =
                static_cast<int>(std::min<std::uint32_t>(latency->get(node->plugin_), std::numeric_limits<int>::max()));
    node->refreshParamMeta();
    return node;
}

ClapAudioNode::~ClapAudioNode()
{
    if (plugin_ != nullptr)
    {
        if (screenOpen_ && screenExt_->on_close != nullptr)
            screenExt_->on_close(plugin_);
        if (processing_ && plugin_->stop_processing != nullptr)
            plugin_->stop_processing(plugin_);
        if (activated_ && plugin_->deactivate != nullptr)
            plugin_->deactivate(plugin_);
        if (plugin_->destroy != nullptr)
            plugin_->destroy(plugin_);
        plugin_ = nullptr;
    }
    delete hostStorage_;
    hostStorage_ = nullptr;
    if (entry_ != nullptr && entry_->deinit != nullptr)
        entry_->deinit();
    entry_ = nullptr;
    if (dlHandle_ != nullptr)
    {
        dlclose(dlHandle_);
        dlHandle_ = nullptr;
    }
}

void ClapAudioNode::refreshParamMeta()
{
    paramsMeta_.clear();
    cachedValues_.clear();
    if (paramsExt_ == nullptr || paramsExt_->count == nullptr || paramsExt_->get_info == nullptr)
        return;
    const std::uint32_t n = paramsExt_->count(plugin_);
    paramsMeta_.reserve(n);
    cachedValues_.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        clap_param_info_t info{};
        if (! paramsExt_->get_info(plugin_, i, &info))
            continue;
        ParamMeta m;
        m.id = info.id;
        m.name = info.name;
        m.minValue = info.min_value;
        m.maxValue = info.max_value;
        m.defaultValue = info.default_value;
        m.stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        m.readOnly = (info.flags & CLAP_PARAM_IS_READONLY) != 0;
        double cur = m.defaultValue;
        if (paramsExt_->get_value != nullptr)
            paramsExt_->get_value(plugin_, info.id, &cur);
        paramsMeta_.push_back(std::move(m));
        cachedValues_.push_back(cur);
    }
}

void ClapAudioNode::setBypassed(bool on) noexcept
{
    bypassed_.store(on, std::memory_order_release);
    if (! on)
        wakeAudio_.store(true, std::memory_order_release);
}

void ClapAudioNode::noteOn(int frame, int note, float velocity) noexcept
{
    if (noteEvCount_ >= static_cast<int>(kMaxNoteEv))
        return;
    noteEvs_[static_cast<std::size_t>(noteEvCount_++)] = {frame, 1, static_cast<std::uint8_t>(note & 127), velocity};
    ++heldNotes_;
    sleeping_.store(false, std::memory_order_relaxed);
    silentBlocks_ = 0;
}

void ClapAudioNode::noteOff(int frame, int note) noexcept
{
    if (noteEvCount_ >= static_cast<int>(kMaxNoteEv))
        return;
    noteEvs_[static_cast<std::size_t>(noteEvCount_++)] = {frame, 2, static_cast<std::uint8_t>(note & 127), 0.0f};
    if (heldNotes_ > 0)
        --heldNotes_;
}

void ClapAudioNode::allNotesOff(int frame) noexcept
{
    if (noteEvCount_ >= static_cast<int>(kMaxNoteEv))
        noteEvCount_ = static_cast<int>(kMaxNoteEv) - 1;
    noteEvs_[static_cast<std::size_t>(noteEvCount_++)] = {frame, 3, 0, 0.0f};
    heldNotes_ = 0;
}

bool ClapAudioNode::active() const noexcept
{
    return ! bypassed_.load(std::memory_order_relaxed) && ! sleeping_.load(std::memory_order_relaxed);
}

bool ClapAudioNode::process(const core::audio::PluginProcessContext& ctx, float* outL, float* outR) noexcept
{
    return processBlock(ctx, outL, outR, nullptr, nullptr);
}

void ClapAudioNode::resetEffect() noexcept
{
    if (plugin_->reset)
        plugin_->reset(plugin_);
}

void ClapAudioNode::processEffect(const core::audio::PluginProcessContext& ctx, float* left, float* right) noexcept
{
    if (ctx.frames < 1 || ctx.frames > engine::kBlockFrames)
        return;
    float* input = left;
    if (inputChannels_ == 1)
    {
        for (int i = 0; i < ctx.frames; ++i)
            silenceL_[i] = (left[i] + right[i]) * 0.5f;
        input = silenceL_.data();
    }
    float wetL[engine::kBlockFrames], wetR[engine::kBlockFrames];
    if (processBlock(ctx, wetL, wetR, input, right))
    {
        std::copy_n(wetL, ctx.frames, left);
        std::copy_n(outputChannels_ == 1 ? wetL : wetR, ctx.frames, right);
    }
}

bool ClapAudioNode::processBlock(const core::audio::PluginProcessContext& ctx, float* outL, float* outR, float* inL,
                                 float* inR) noexcept
{
    if (plugin_ == nullptr || plugin_->process == nullptr || ctx.frames <= 0 || ctx.frames > engine::kBlockFrames)
    {
        noteEvCount_ = 0;
        return false;
    }

    const bool hasNotes = noteEvCount_ > 0;
    const bool hasParams = paramHead_.load(std::memory_order_acquire) != paramTail_.load(std::memory_order_relaxed);
    const bool woke = wakeAudio_.exchange(false, std::memory_order_acq_rel);

    const bool bypass = bypassed_.load(std::memory_order_acquire);
    if (bypass && ! hasParams)
    {
        noteEvCount_ = 0;
        return false;
    }
    if (! inL && sleeping_.load(std::memory_order_relaxed) && ! hasNotes && ! hasParams && ! woke && ! ctx.playing)
        return false;

    sleeping_.store(false, std::memory_order_relaxed);

    BlockEventList inEvents;
    // Drain SPSC param queue at frame 0
    std::size_t tail = paramTail_.load(std::memory_order_relaxed);
    const std::size_t head = paramHead_.load(std::memory_order_acquire);
    while (tail != head && inEvents.count < BlockEventList::kMax - static_cast<std::size_t>(noteEvCount_))
    {
        const auto& ev = paramQueue_[tail];
        inEvents.pushParam(0, ev.id, ev.value);
        tail = (tail + 1) & (kParamQueueSize - 1);
    }
    if (bypass)
    {
        DiscardOutEvents out;
        if (paramsExt_ && paramsExt_->flush)
            paramsExt_->flush(plugin_, &inEvents.iface, &out.iface);
        paramTail_.store(tail, std::memory_order_release);
        noteEvCount_ = 0;
        return false;
    }

    // Push per-block notes
    for (int i = 0; i < noteEvCount_; ++i)
    {
        const auto& ne = noteEvs_[static_cast<std::size_t>(i)];
        const auto t =
            static_cast<std::uint32_t>(ne.frame < 0 ? 0 : (ne.frame >= ctx.frames ? ctx.frames - 1 : ne.frame));
        if (ne.kind == 1)
            inEvents.pushNote(CLAP_EVENT_NOTE_ON, t, ne.note, static_cast<double>(ne.velocity));
        else if (ne.kind == 2)
            inEvents.pushNote(CLAP_EVENT_NOTE_OFF, t, ne.note, 0.0);
        else if (ne.kind == 3)
            inEvents.pushNote(CLAP_EVENT_NOTE_CHOKE, t, -1, 0.0);
    }
    noteEvCount_ = 0;

    std::memset(outL, 0, sizeof(float) * static_cast<std::size_t>(ctx.frames));
    std::memset(outR, 0, sizeof(float) * static_cast<std::size_t>(ctx.frames));

    float* inPtrs[2] = {inL ? inL : silenceL_.data(), inR ? inR : silenceR_.data()};
    float* outPtrs[2] = {outL, outR};
    clap_audio_buffer_t inBuf{};
    inBuf.data32 = inPtrs;
    inBuf.channel_count = inputChannels_;
    inBuf.latency = 0;
    inBuf.constant_mask = inL ? 0u : (1u << inputChannels_) - 1u;

    clap_audio_buffer_t outBuf{};
    outBuf.data32 = outPtrs;
    outBuf.channel_count = outputChannels_;
    outBuf.latency = 0;
    outBuf.constant_mask = 0;

    clap_event_transport_t transport{};
    transport.header.size = sizeof(clap_event_transport_t);
    transport.header.time = 0;
    transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    transport.header.type = CLAP_EVENT_TRANSPORT;
    transport.flags = static_cast<std::uint32_t>(CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE) |
                      (ctx.playing ? static_cast<std::uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u);
    transport.tempo = ctx.bpm > 0.0 ? ctx.bpm : 120.0;
    transport.song_pos_beats =
        static_cast<clap_beattime>(std::llround(ctx.ppqStart * static_cast<double>(CLAP_BEATTIME_FACTOR)));
    transport.bar_number = static_cast<std::int32_t>(std::floor(ctx.ppqStart * 0.25));
    transport.bar_start = static_cast<clap_beattime>(transport.bar_number * 4LL * CLAP_BEATTIME_FACTOR);
    transport.tsig_num = 4;
    transport.tsig_denom = 4;

    DiscardOutEvents outEvents;
    clap_process_t proc{};
    proc.steady_time = -1;
    proc.frames_count = static_cast<std::uint32_t>(ctx.frames);
    proc.transport = &transport;
    proc.audio_inputs = inputChannels_ ? &inBuf : nullptr;
    proc.audio_outputs = &outBuf;
    proc.audio_inputs_count = inputChannels_ ? 1u : 0u;
    proc.audio_outputs_count = 1;
    proc.in_events = &inEvents.iface;
    proc.out_events = &outEvents.iface;

    const clap_process_status st = plugin_->process(plugin_, &proc);
    paramTail_.store(tail, std::memory_order_release);

    if (st == CLAP_PROCESS_ERROR)
        return false;
    if (inL)
        return true;
    if (outputChannels_ == 1)
        std::copy_n(outL, ctx.frames, outR);

    float peak = 0.0f;
    for (int i = 0; i < ctx.frames; ++i)
    {
        const float al = std::fabs(outL[i]);
        const float ar = std::fabs(outR[i]);
        if (al > peak)
            peak = al;
        if (ar > peak)
            peak = ar;
    }

    if (peak <= kSilencePeak && heldNotes_ == 0 && ! ctx.playing)
    {
        if (st == CLAP_PROCESS_SLEEP || ++silentBlocks_ >= kIdleSilenceBlocks)
            sleeping_.store(true, std::memory_order_relaxed);
    }
    else
        silentBlocks_ = 0;

    return peak > 0.0f;
}

bool ClapAudioNode::pushParamChange(std::uint32_t id, double value) noexcept
{
    const std::size_t head = paramHead_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & (kParamQueueSize - 1);
    if (next == paramTail_.load(std::memory_order_acquire))
        return false;
    paramQueue_[head] = {id, value};
    paramHead_.store(next, std::memory_order_release);
    wakeAudio_.store(true, std::memory_order_release);
    return true;
}

const ParamMeta* ClapAudioNode::paramById(std::uint32_t id) const noexcept
{
    for (const auto& m : paramsMeta_)
        if (m.id == id)
            return &m;
    return nullptr;
}

const ParamMeta* ClapAudioNode::paramByIndex(std::size_t idx) const noexcept
{
    return idx < paramsMeta_.size() ? &paramsMeta_[idx] : nullptr;
}

double ClapAudioNode::paramValue(std::uint32_t id) const noexcept
{
    if (paramsExt_ != nullptr && paramsExt_->get_value != nullptr)
    {
        double v = 0.0;
        if (paramsExt_->get_value(plugin_, id, &v))
            return v;
    }
    for (std::size_t i = 0; i < paramsMeta_.size(); ++i)
        if (paramsMeta_[i].id == id)
            return cachedValues_[i];
    return 0.0;
}

bool ClapAudioNode::formatParamValue(std::uint32_t id, double value, char* buf, std::size_t len) const noexcept
{
    if (buf == nullptr || len == 0)
        return false;
    if (paramsExt_ != nullptr && paramsExt_->value_to_text != nullptr &&
        paramsExt_->value_to_text(plugin_, id, value, buf, static_cast<std::uint32_t>(len)))
        return true;
    std::snprintf(buf, len, "%.2f", value);
    return true;
}

bool ClapAudioNode::setParamValue(std::uint32_t id, double value) noexcept
{
    const ParamMeta* meta = nullptr;
    std::size_t index = paramsMeta_.size();
    for (std::size_t i = 0; i < paramsMeta_.size(); ++i)
        if (paramsMeta_[i].id == id)
        {
            meta = &paramsMeta_[i];
            index = i;
            break;
        }
    if (meta == nullptr || meta->readOnly)
        return false;
    double clamped = std::clamp(value, meta->minValue, meta->maxValue);
    if (meta->stepped)
        clamped = std::round(clamped);
    if (! parameterEditPending())
        uiParamEdits_.clear();
    if (! pushParamChange(id, clamped))
        return false;
    cachedValues_[index] = clamped;
    auto edit = std::find_if(uiParamEdits_.begin(), uiParamEdits_.end(), [id](const auto& e) { return e.id == id; });
    if (edit == uiParamEdits_.end())
        uiParamEdits_.push_back({id, clamped, clamped});
    else
        edit->target = clamped;
    return true;
}

int ClapAudioNode::pageCount() const noexcept
{
    return paramsMeta_.empty() ? 1 : static_cast<int>((paramsMeta_.size() + 5) / 6);
}

void ClapAudioNode::setPage(int page) noexcept
{
    page_ = std::clamp(page, 0, std::max(0, pageCount() - 1));
}

bool ClapAudioNode::readKnobs(std::array<KnobReadout, 6>& out, char outTab[SIXTEEN_TAB_NAME_SIZE]) const noexcept
{
    out = {};
    copyTrunc(outTab, SIXTEEN_TAB_NAME_SIZE, name_.empty() ? std::string_view("PLUGIN") : std::string_view(name_));

    std::array<std::uint32_t, 6> ids{
        SIXTEEN_INVALID_PARAM_ID, SIXTEEN_INVALID_PARAM_ID, SIXTEEN_INVALID_PARAM_ID,
        SIXTEEN_INVALID_PARAM_ID, SIXTEEN_INVALID_PARAM_ID, SIXTEEN_INVALID_PARAM_ID,
    };

    bool customKnobs = false;
    openScreen();
    if (screenExt_ != nullptr && screenExt_->knobs != nullptr)
    {
        char tabBuf[SIXTEEN_TAB_NAME_SIZE]{};
        if (screenExt_->knobs(plugin_, ids.data(), tabBuf))
        {
            customKnobs = true;
            if (tabBuf[0] != '\0')
                copyTrunc(outTab, SIXTEEN_TAB_NAME_SIZE, tabBuf);
        }
    }

    if (! customKnobs)
    {
        const std::size_t base = static_cast<std::size_t>(std::max(0, page_)) * 6;
        for (std::size_t c = 0; c < 6; ++c)
            if (base + c < paramsMeta_.size())
                ids[c] = paramsMeta_[base + c].id;
    }

    bool any = false;
    for (std::size_t c = 0; c < 6; ++c)
    {
        if (ids[c] == SIXTEEN_INVALID_PARAM_ID)
            continue;
        const ParamMeta* meta = paramById(ids[c]);
        if (meta == nullptr)
            continue;
        auto& k = out[c];
        k.active = true;
        k.stepped = meta->stepped;
        k.bipolar = meta->minValue < 0.0 && meta->maxValue > 0.0;
        k.paramId = meta->id;
        const double v = paramValue(meta->id);
        const double span = meta->maxValue - meta->minValue;
        k.norm = span > 0.0 ? static_cast<float>(std::clamp((v - meta->minValue) / span, 0.0, 1.0)) : 0.0f;
        copyTrunc(k.label, sizeof(k.label), meta->name);
        formatParamValue(meta->id, v, k.valueText, sizeof(k.valueText));
        any = true;
    }
    return any;
}

bool ClapAudioNode::turnKnob(int col0, int detents, bool fine, ClapAudioNode* stateEditor) noexcept
{
    if (col0 < 0 || col0 >= 6 || detents == 0)
        return false;
    std::array<KnobReadout, 6> bank{};
    char tab[SIXTEEN_TAB_NAME_SIZE]{};
    if (! readKnobs(bank, tab) || ! bank[static_cast<std::size_t>(col0)].active)
        return false;
    const ParamMeta* meta = paramById(bank[static_cast<std::size_t>(col0)].paramId);
    if (meta == nullptr || meta->readOnly)
        return false;
    double cur = stateEditor ? stateEditor->paramValue(meta->id) : paramValue(meta->id);
    if (! stateEditor && parameterEditPending())
        for (const auto& edit : uiParamEdits_)
            if (edit.id == meta->id)
                cur = edit.target;
    double next = cur;
    if (meta->stepped)
        next = std::round(cur) + static_cast<double>(detents);
    else
    {
        const double span = meta->maxValue - meta->minValue;
        const double step = span * (fine ? 0.005 : 0.025);
        next = cur + static_cast<double>(detents) * step;
    }
    next = std::clamp(next, meta->minValue, meta->maxValue);
    if (next == cur || ! setParamValue(meta->id, next))
        return false;
    // The project needs the intended state even if Save or Undo precedes the next audio block.
    // CLAP permits flush on the main thread for this inactive, private copy only.
    if (stateEditor)
        stateEditor->flushInactiveParam(meta->id, next);
    return true;
}

void ClapAudioNode::flushInactiveParam(std::uint32_t id, double value)
{
    if (activated_ || ! paramsExt_ || ! paramsExt_->flush)
        return;
    BlockEventList event;
    DiscardOutEvents out;
    event.pushParam(0, id, value);
    paramsExt_->flush(plugin_, &event.iface, &out.iface);
}

void ClapAudioNode::beforeScreenInput()
{
    if (! parameterEditPending())
        uiParamEdits_.clear();
    for (auto& edit : uiParamEdits_)
        edit.beforeInput = paramValue(edit.id);
}

void ClapAudioNode::afterScreenInput()
{
    for (auto& edit : uiParamEdits_)
    {
        const auto value = paramValue(edit.id);
        if (value != edit.beforeInput && value != edit.target)
        {
            // A touch on the same parameter supersedes its queued encoder turn.
            // Queue the newer value without changing the list being iterated.
            if (pushParamChange(edit.id, value))
                edit.target = value;
        }
    }
}

std::string ClapAudioNode::editorState(ClapAudioNode* stateEditor) const
{
    // Test the queue before taking the snapshot: an edit completing between those reads
    // is already present, and applying its absolute value again has the same result.
    if (uiParamEdits_.empty() || ! parameterEditPending())
        return saveStateBase64();
    std::unique_ptr<ClapAudioNode> mirror;
    if (! stateEditor)
    {
        std::string error;
        mirror.reset(create(absPath_, id_, sampleRate_, error, false, true));
        stateEditor = mirror.get();
    }
    if (! stateEditor)
        return {};
    auto state = saveStateBase64();
    if (! stateEditor->loadStateBase64(state))
        return {};
    for (const auto& edit : uiParamEdits_)
        if (stateEditor->paramValue(edit.id) != edit.target)
            stateEditor->flushInactiveParam(edit.id, edit.target);
    return stateEditor->saveStateBase64();
}

bool ClapAudioNode::pushKnob(int col0) noexcept
{
    if (col0 < 0 || col0 >= 6)
        return false;
    std::array<KnobReadout, 6> bank{};
    char tab[SIXTEEN_TAB_NAME_SIZE]{};
    if (! readKnobs(bank, tab) || ! bank[static_cast<std::size_t>(col0)].active)
        return false;
    const ParamMeta* meta = paramById(bank[static_cast<std::size_t>(col0)].paramId);
    if (meta == nullptr || meta->readOnly)
        return false;
    return setParamValue(meta->id, meta->defaultValue);
}

void ClapAudioNode::openScreen() const noexcept
{
    if (screenExt_ && ! screenOpen_)
    {
        screenOpen_ = true;
        if (screenExt_->on_open)
            screenExt_->on_open(plugin_);
    }
}

ScreenBitmap ClapAudioNode::renderScreen(std::uint32_t nowMs) noexcept
{
    if (! screenExt_)
        return {};
    openScreen();
    if (! screenBuf_)
    {
        constexpr auto count = SIXTEEN_SCREEN_WIDTH * SIXTEEN_SCREEN_HEIGHT;
        screenBuf_.reset(new (std::nothrow) std::uint32_t[count]);
        if (! screenBuf_)
            return {};
        std::fill_n(screenBuf_.get(), count, 0xff0b0e11u);
    }
    sixteen_screen_surface_t surf{};
    surf.pixels = screenBuf_.get();
    surf.width = SIXTEEN_SCREEN_WIDTH;
    surf.height = SIXTEEN_SCREEN_HEIGHT;
    surf.stride = SIXTEEN_SCREEN_WIDTH;
    surf.now_ms = nowMs;
    const bool forceFull = ! screenInitialized_;
    if (screenExt_->render(plugin_, &surf, forceFull) || forceFull)
    {
        screenInitialized_ = true;
        ++screenSerial_;
    }
    return {screenBuf_.get(), SIXTEEN_SCREEN_WIDTH, SIXTEEN_SCREEN_HEIGHT, SIXTEEN_SCREEN_WIDTH, screenSerial_};
}

bool ClapAudioNode::touchScreen(int phase, int x, int y) noexcept
{
    if (screenExt_ == nullptr || screenExt_->touch == nullptr)
        return false;
    return screenExt_->touch(plugin_, phase, x, y);
}

bool ClapAudioNode::selectorTurn(int delta) noexcept
{
    if (screenExt_ == nullptr || screenExt_->selector_turn == nullptr)
        return false;
    return screenExt_->selector_turn(plugin_, delta);
}

bool ClapAudioNode::selectorPush() noexcept
{
    if (screenExt_ == nullptr || screenExt_->selector_push == nullptr)
        return false;
    return screenExt_->selector_push(plugin_);
}

std::string ClapAudioNode::saveStateBase64() const
{
    if (! stateExt_ || ! stateExt_->save || ! stateExt_->load)
    {
        juce::Array<juce::var> values;
        if (paramsExt_ && paramsExt_->flush)
            for (const auto& param : paramsMeta_)
                if (! param.readOnly)
                    values.add(juce::var(
                        juce::Array<juce::var>{juce::var(static_cast<juce::int64>(param.id)), paramValue(param.id)}));
        return values.isEmpty()
                   ? std::string{}
                   : std::string(kParamsPrefix) + juce::JSON::toString(juce::var(values), true, 17).toStdString();
    }
    juce::MemoryBlock bytes;
    clap_ostream_t stream{};
    stream.ctx = &bytes;
    stream.write = [](const clap_ostream_t* s, const void* buf, std::uint64_t size) -> std::int64_t
    {
        if (s == nullptr || s->ctx == nullptr || buf == nullptr)
            return -1;
        static_cast<juce::MemoryBlock*>(s->ctx)->append(buf, static_cast<std::size_t>(size));
        return static_cast<std::int64_t>(size);
    };
    if (! stateExt_->save(plugin_, &stream) || bytes.getSize() == 0)
        return {};
    return juce::Base64::toBase64(bytes.getData(), bytes.getSize()).toStdString();
}

bool ClapAudioNode::loadStateBase64(std::string_view b64)
{
    if (b64.starts_with(kParamsPrefix))
    {
        if (activated_ || ! paramsExt_ || ! paramsExt_->flush)
            return false;
        b64.remove_prefix(kParamsPrefix.size());
        auto data = juce::JSON::parse(juce::String::fromUTF8(b64.data(), static_cast<int>(b64.size())));
        const auto* values = data.getArray();
        if (! values)
            return false;
        std::vector<ParamEv> edits;
        for (const auto& entry : *values)
        {
            const auto* pair = entry.getArray();
            if (! pair || pair->size() != 2)
                return false;
            const auto& id = (*pair)[0];
            const auto& v = (*pair)[1];
            const auto number = static_cast<juce::int64>(id);
            if ((! id.isInt() && ! id.isInt64()) || number < 0 || number > UINT32_MAX ||
                (! v.isDouble() && ! v.isInt() && ! v.isInt64()) || ! std::isfinite(static_cast<double>(v)))
                return false;
            const auto* param = paramById(static_cast<std::uint32_t>(number));
            if (param && ! param->readOnly)
                edits.push_back({param->id, std::clamp(static_cast<double>(v), param->minValue, param->maxValue)});
        }
        for (const auto& edit : edits)
            flushInactiveParam(edit.id, edit.value);
        return ! edits.empty();
    }
    if (b64.empty() || stateExt_ == nullptr || stateExt_->load == nullptr || plugin_ == nullptr)
        return false;
    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64(decoded, juce::String::fromUTF8(b64.data(), static_cast<int>(b64.size()))))
        return false;
    struct ReadCtx
    {
        const std::uint8_t* data;
        std::size_t size;
        std::size_t pos;
    } ctx{static_cast<const std::uint8_t*>(decoded.getData()), decoded.getDataSize(), 0};
    clap_istream_t stream{};
    stream.ctx = &ctx;
    stream.read = [](const clap_istream_t* s, void* buf, std::uint64_t size) -> std::int64_t
    {
        auto* c = static_cast<ReadCtx*>(s->ctx);
        const std::size_t avail = c->size - c->pos;
        const std::size_t n = std::min(avail, static_cast<std::size_t>(size));
        if (n > 0)
        {
            std::memcpy(buf, c->data + c->pos, n);
            c->pos += n;
        }
        return static_cast<std::int64_t>(n);
    };
    if (! stateExt_->load(plugin_, &stream))
        return false;
    refreshParamMeta();
    wakeAudio_.store(true, std::memory_order_release);
    return true;
}

void ClapAudioNode::pollMainThread() noexcept
{
    if (plugin_ == nullptr)
        return;
    if (callbackRequested_.exchange(false, std::memory_order_acq_rel) && plugin_->on_main_thread != nullptr)
        plugin_->on_main_thread(plugin_);
    if (flushRequested_.exchange(false, std::memory_order_acq_rel) && paramsExt_ != nullptr &&
        paramsExt_->flush != nullptr && sleeping_.load(std::memory_order_relaxed))
    {
        BlockEventList emptyIn;
        DiscardOutEvents emptyOut;
        paramsExt_->flush(plugin_, &emptyIn.iface, &emptyOut.iface);
    }
}

// ---- ClapHostService ------------------------------------------------------------------------------------------------

ClapHostService::ClapHostService(engine::EngineBridge& bridge, std::string pluginsDir, double sampleRate,
                                 std::function<void(const std::string&)> log)
    : bridge_(&bridge), pluginsDir_(std::move(pluginsDir)), sampleRate_(sampleRate), log_(std::move(log))
{
}

ClapHostService::~ClapHostService()
{
    unmountAll();
    // A full bridge may still own the engine reference; release our independent reference.
    for (auto& slot : slots_)
        if (slot.node)
            slot.node->release();
    for (auto& slot : effects_)
        if (slot.node)
            slot.node->release();
}

void ClapHostService::rescan()
{
    catalogue_ = scanPluginDirectory(pluginsDir_, sampleRate_);
    ++changes_;
    if (log_)
        log_("PLUGIN scan dir=" + pluginsDir_ + " found=" + std::to_string(catalogue_.size()));
}

std::string ClapHostService::resolvePath(const std::string& path) const
{
    if (path.empty())
        return {};
    if (juce::File::isAbsolutePath(path))
    {
        const juce::File f(path);
        if (f.existsAsFile() || f.isDirectory())
            return f.getFullPathName().toStdString();
    }
    if (! pluginsDir_.empty())
        return juce::File(pluginsDir_).getChildFile(path).getFullPathName().toStdString();
    return path;
}

bool ClapHostService::mount(int scene0, int track0, const core::model::PluginTrack& spec)
{
    return mountSlot(
        slots_[idx(scene0, track0)],
        {engine::SwapSlot::TrackPlugin, static_cast<std::uint8_t>(scene0 & 15), static_cast<std::uint8_t>(track0 & 15)},
        spec);
}

ClapAudioNode* ClapHostService::mountedEffect(int scene0, int slot) const noexcept
{
    if (scene0 < 0 || scene0 >= 16 || slot < 0 || slot >= engine::kPluginFxSlots)
        return nullptr;
    const auto& current = effects_[scene0 * engine::kPluginFxSlots + slot];
    return current.pending || current.pendingClear ? nullptr : current.node;
}

void ClapHostService::acknowledgeEffectEdit(int scene0, int slot, const std::string& state)
{
    if (mountedEffect(scene0, slot))
        effects_[scene0 * engine::kPluginFxSlots + slot].spec.stateBase64 = state;
}

bool ClapHostService::mountEffect(int scene0, int slot, const core::model::PluginTrack& spec)
{
    if (scene0 < 0 || scene0 >= 16 || slot < 0 || slot >= engine::kPluginFxSlots)
        return false;
    return mountSlot(effects_[scene0 * engine::kPluginFxSlots + slot], engine::pluginFxSwap(scene0, slot), spec);
}

bool ClapHostService::mountSlot(Slot& slot, engine::SwapSlot swapSlot, const core::model::PluginTrack& spec,
                                bool recall, bool fresh, bool strictPreset)
{
    if (recall && ! fresh && slot.pending && slot.pending->spec == spec)
        return true;
    if (recall)
        slot.pending.reset();
    if (spec.path.empty() && spec.pluginId.empty())
    {
        clearSlot(slot, swapSlot);
        return ! slot.pendingClear;
    }

    // Undo to the initial sound of a stateless plugin needs a fresh instance.
    const bool needsInitialSound = spec.stateBase64.empty() && spec.presetKey.empty() &&
                                   (! slot.spec.stateBase64.empty() || ! slot.spec.presetKey.empty());
    if (slot.node && slot.node->parameterEditPending() && slot.spec.stateBase64 != spec.stateBase64)
        fresh = true; // an older queued edit must never land after Undo
    const bool parameterPatch = spec.stateBase64.starts_with(kParamsPrefix);
    if (parameterPatch && spec.stateBase64 != slot.spec.stateBase64)
        fresh = true;
    if (! fresh && slot.node != nullptr && slot.spec.path == spec.path && slot.spec.pluginId == spec.pluginId &&
        ! needsInitialSound)
    {
        slot.pending.reset();
        slot.pendingClear = false;
        slot.node->setBypassed(spec.bypass);
        if (! spec.stateBase64.empty() &&
            (spec.stateBase64 != slot.spec.stateBase64 || spec.presetKey != slot.spec.presetKey))
            slot.node->loadStateBase64(spec.stateBase64);
        else if (spec.stateBase64.empty() && spec.presetKey != slot.spec.presetKey &&
                 ! slot.node->loadBundledPreset(spec.presetKey))
            return false;
        slot.spec = spec;
        ++changes_;
        return true;
    }

    std::string absPath = resolvePath(spec.path);
    if (absPath.empty() && ! spec.pluginId.empty())
    {
        for (const auto& e : catalogue_)
            if (e.id == spec.pluginId)
            {
                absPath = e.absPath;
                break;
            }
    }
    std::string err;
    ClapAudioNode* node =
        ClapAudioNode::create(absPath, spec.pluginId, sampleRate_, err, swapSlot.kind != engine::SwapSlot::TrackPlugin,
                              false, parameterPatch ? std::string_view(spec.stateBase64) : std::string_view{});
    auto location = [swapSlot]
    {
        const int fx = engine::pluginFxIndex(swapSlot);
        return " s=" + std::to_string(swapSlot.scene + 1) +
               (fx < 0 ? " t=" + std::to_string(swapSlot.index + 1) : " fxSlot=" + std::to_string(fx));
    };
    if (node == nullptr)
    {
        if (log_)
            log_("PLUGIN mount failed" + location() + " path=" + absPath + " err=" + err);
        return false;
    }
    const bool restoredState =
        parameterPatch || (! spec.stateBase64.empty() && node->loadStateBase64(spec.stateBase64));
    const bool missingPreset = ! restoredState && ! spec.presetKey.empty() && ! node->loadBundledPreset(spec.presetKey);
    if (missingPreset && log_)
        log_("PLUGIN preset unavailable id=" + node->id() + " key=" + spec.presetKey);
    node->setBypassed(spec.bypass);

    PendingMount prepared{std::unique_ptr<ClapAudioNode>(node), spec};
    if (strictPreset && (missingPreset || (! spec.stateBase64.empty() && ! restoredState)))
        return false;
    if (strictPreset && spec.stateBase64.empty())
        prepared.spec.stateBase64 = node->saveStateBase64();
    if (missingPreset)
        prepared.spec.presetKey.clear();
    if (prepared.spec.name.empty())
        prepared.spec.name = node->name();
    if (postMount(slot, swapSlot, prepared))
    {
        slot.pending.reset();
        return true;
    }
    if (! recall)
        return false;
    slot.pending = std::make_unique<PendingMount>(std::move(prepared));
    slot.pendingClear = false;
    return true;
}

bool ClapHostService::postMount(Slot& slot, engine::SwapSlot swapSlot, PendingMount& prepared)
{
    auto* node = prepared.node.get();
    node->retain(); // the swap table owns this reference once post succeeds
    if (bridge_ == nullptr || ! bridge_->post(engine::ObjectSwap{swapSlot, node}))
    {
        node->release();
        return false;
    }
    if (slot.node)
        slot.node->release();
    slot.node = prepared.node.release();
    slot.spec = std::move(prepared.spec);
    slot.pendingClear = false;
    ++changes_;
    if (log_)
    {
        const int fx = engine::pluginFxIndex(swapSlot);
        log_("PLUGIN mounted s=" + std::to_string(swapSlot.scene + 1) +
             (fx < 0 ? " t=" + std::to_string(swapSlot.index + 1) : " fxSlot=" + std::to_string(fx)) +
             " id=" + node->id() + " screen=" + (node->hasCustomScreen() ? "custom" : "generic"));
    }
    return true;
}

bool ClapHostService::recallPreset(int scene0, int track0, std::optional<int> effectSlot,
                                   const core::model::PluginTrack& spec)
{
    if (scene0 < 0 || scene0 >= 16 || track0 < 0 || track0 >= 16 ||
        (effectSlot && (*effectSlot < 0 || *effectSlot >= engine::kPluginFxSlots)))
        return false;
    const auto swap = effectSlot ? engine::pluginFxSwap(scene0, *effectSlot)
                                 : engine::SwapSlot{engine::SwapSlot::TrackPlugin, static_cast<std::uint8_t>(scene0),
                                                    static_cast<std::uint8_t>(track0)};
    auto& slot = effectSlot ? effects_[scene0 * engine::kPluginFxSlots + *effectSlot] : slots_[idx(scene0, track0)];
    // Validate on a replacement instance before publishing. Pending edits on the old instance cannot leak into it.
    return slot.node && slot.node->id() == spec.pluginId && mountSlot(slot, swap, spec, false, true, true);
}

void ClapHostService::unmount(int scene0, int track0)
{
    clearSlot(slots_[idx(scene0, track0)], {engine::SwapSlot::TrackPlugin, static_cast<std::uint8_t>(scene0 & 15),
                                            static_cast<std::uint8_t>(track0 & 15)});
}

void ClapHostService::clearSlot(Slot& slot, engine::SwapSlot swapSlot)
{
    slot.pending.reset();
    if (slot.node == nullptr && slot.spec.path.empty() && slot.spec.pluginId.empty())
        return;
    if (bridge_ != nullptr && ! bridge_->post(engine::ObjectSwap{swapSlot, nullptr}))
    {
        slot.pendingClear = true;
        return;
    }
    slot.pendingClear = false;
    if (slot.node != nullptr)
    {
        slot.node->release();
        slot.node = nullptr;
    }
    slot.spec = {};
    ++changes_;
}

void ClapHostService::unmountAll()
{
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
            unmount(s, t);
    for (int s = 0; s < 16; ++s)
        for (int i = 0; i < engine::kPluginFxSlots; ++i)
            clearSlot(effects_[s * engine::kPluginFxSlots + i], engine::pluginFxSwap(s, i));
}

ClapAudioNode* ClapHostService::mounted(int scene0, int track0) const noexcept
{
    const auto& slot = slots_[idx(scene0, track0)];
    return slot.pending || slot.pendingClear ? nullptr : slot.node;
}

const core::model::PluginTrack& ClapHostService::mountedSpec(int scene0, int track0,
                                                             std::optional<int> effectSlot) const noexcept
{
    if (effectSlot && (scene0 < 0 || scene0 >= 16 || *effectSlot < 0 || *effectSlot >= engine::kPluginFxSlots))
    {
        static const core::model::PluginTrack empty;
        return empty;
    }
    return effectSlot ? effects_[scene0 * engine::kPluginFxSlots + *effectSlot].spec : slots_[idx(scene0, track0)].spec;
}

void ClapHostService::syncEffect(const core::model::Project& project, int scene, int slot, bool fresh)
{
    const auto& fx = effectAt(project, scene, slot);
    auto& current = effects_[scene * engine::kPluginFxSlots + slot];
    if (fx.type == core::model::EffectType::Plugin)
    {
        if (! fresh && ! current.pending && current.node && ! current.pendingClear && current.spec == fx.plugin)
            return;
        if (mountSlot(current, engine::pluginFxSwap(scene, slot), fx.plugin, true, fresh))
            return;
    }
    clearSlot(current, engine::pluginFxSwap(scene, slot));
}

bool ClapHostService::moveTrackPlugins(const core::TrackSwap& swap)
{
    const auto slotsFor = [this, scene = swap.scene](int track)
    {
        const int insert = scene * engine::kPluginFxSlots + 2 * track;
        return std::array{&slots_[idx(scene, track)], &effects_[insert], &effects_[insert + 1]};
    };
    const auto first = slotsFor(swap.first);
    const auto second = slotsFor(swap.second);
    const auto hasPlugin = [](const Slot* slot) { return slot->node || slot->pending; };
    if (! std::any_of(first.begin(), first.end(), hasPlugin) && ! std::any_of(second.begin(), second.end(), hasPlugin))
        return false;
    for (std::size_t i = 0; i < first.size(); ++i)
        std::swap(*first[i], *second[i]);
    ++changes_;
    return true;
}

void ClapHostService::syncFromProject(const core::model::Project& project)
{
    for (int s = 0; s < 16; ++s)
        for (int i = 0; i < engine::kPluginFxSlots; ++i)
            syncEffect(project, s, i, true);
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
        {
            syncTrack(project, s, t, true);
        }
}

void ClapHostService::syncTrack(const core::model::Project& project, int s, int t, bool fresh)
{
    const auto& tr = project.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)];
    if (tr.mode == core::model::TrackMode::Plugin && (! tr.plugin.path.empty() || ! tr.plugin.pluginId.empty()))
    {
        auto& slot = slots_[idx(s, t)];
        if (! fresh && ! slot.pending && ! slot.pendingClear && slot.node && slot.spec == tr.plugin)
            return;
        engine::SwapSlot swap{engine::SwapSlot::TrackPlugin, static_cast<std::uint8_t>(s),
                              static_cast<std::uint8_t>(t)};
        if (! mountSlot(slot, swap, tr.plugin, true, fresh))
            clearSlot(slot, swap);
    }
    else
        unmount(s, t);
}

void ClapHostService::syncChanges(const core::ChangeSet& changes, const core::model::Project& project)
{
    for (const auto& path : changes.paths())
    {
        if (path.empty() || path == "project")
        {
            syncFromProject(project);
            return;
        }
        std::string_view rest(path);
        int s = 0, t = 0, unit = 0;
        if (! takeIndex(rest, "scene[", s))
            continue;
        if (rest.empty())
        {
            for (int i = 0; i < engine::kPluginFxSlots; ++i)
                syncEffect(project, s, i, true);
            for (int track = 0; track < 16; ++track)
                syncTrack(project, s, track, true);
            continue;
        }
        if (rest == ".sendFx" || rest == ".masterFx")
        {
            syncEffect(project, s, 2 * engine::kTracks + (rest == ".masterFx" ? 1 : 0));
            continue;
        }
        if (! takeIndex(rest, ".track[", t))
            continue;
        if (takeIndex(rest, ".insertFx[", unit) && unit < 2)
        {
            syncEffect(project, s, 2 * t + unit);
            continue;
        }
        if (rest.empty())
        {
            syncEffect(project, s, 2 * t);
            syncEffect(project, s, 2 * t + 1);
        }
        else if (rest != ".plugin" && rest != ".mode")
            continue;
        syncTrack(project, s, t);
    }
}

void ClapHostService::snapshotStates(core::model::Project& project) const
{
    for (int s = 0; s < 16; ++s)
        for (int i = 0; i < engine::kPluginFxSlots; ++i)
        {
            auto& fx = effectAt(project, s, i);
            const auto& slot = effects_[s * engine::kPluginFxSlots + i];
            if (fx.type == core::model::EffectType::Plugin && ! slot.pending && ! slot.pendingClear && slot.node &&
                ! slot.node->parameterEditPending() && slot.spec.path == fx.plugin.path &&
                slot.spec.pluginId == fx.plugin.pluginId)
                if (auto state = slot.node->saveStateBase64(); ! state.empty())
                    fx.plugin.stateBase64 = std::move(state);
        }
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
        {
            const auto& slot = slots_[idx(s, t)];
            auto& tr = project.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)];
            if (tr.mode != core::model::TrackMode::Plugin || ! mounted(s, t) || slot.spec.path != tr.plugin.path ||
                slot.spec.pluginId != tr.plugin.pluginId)
                continue;
            if (auto state = slot.node->editorState(nullptr); ! state.empty())
                tr.plugin.stateBase64 = std::move(state);
        }
}

void ClapHostService::pollMainThread()
{
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
    {
        auto& slot = slots_[i];
        if (slot.pending)
        {
            engine::SwapSlot swap{engine::SwapSlot::TrackPlugin, static_cast<std::uint8_t>(i / 16),
                                  static_cast<std::uint8_t>(i % 16)};
            if (postMount(slot, swap, *slot.pending))
                slot.pending.reset();
        }
        else if (slot.pendingClear)
            unmount(i / 16, i % 16);
        else if (slot.node)
            slot.node->pollMainThread();
    }
    for (int i = 0; i < static_cast<int>(effects_.size()); ++i)
    {
        auto& slot = effects_[i];
        const auto swap = engine::pluginFxSwap(i / engine::kPluginFxSlots, i % engine::kPluginFxSlots);
        if (slot.pending)
        {
            if (postMount(slot, swap, *slot.pending))
                slot.pending.reset();
        }
        else if (slot.pendingClear)
            clearSlot(slot, swap);
        else if (slot.node)
            slot.node->pollMainThread();
    }
}
} // namespace services::plugin
