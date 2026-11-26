// Dynamic CLAP plugin loader and host service (message thread + lock-free RT audio node).
//
// Scans <data>/Plugins/ recursively for *.clap binaries, loads plugins via dlopen / clap_entry,
// wraps each mounted instance in a ref-counted ClapAudioNode (core::audio::PluginAudioNode) swapped
// into Engine via SwapSlot::TrackPlugin, and bridges CLAP parameters, state blobs, and the optional
// custom body screen extension (org.sixteen.screen/1) to the UI layer.
#pragma once

#include "core/audio/PluginNode.h"
#include "core/model/Track.h"
#include "core/util/EmbeddedScreenExt.h"
#include "engine/EngineApi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct clap_plugin_entry;
struct clap_host;
struct clap_plugin;
struct clap_plugin_params;
struct clap_plugin_state;
struct clap_plugin_preset_load;

namespace core
{
class ChangeSet;
namespace model
{
struct Project;
}
} // namespace core

namespace services::plugin
{
struct CatalogueEntry
{
    std::string id{};
    std::string name{};
    std::string vendor{};
    std::string path{};    // relative to pluginsDir (or absolute if outside)
    std::string absPath{}; // resolved binary path passed to dlopen
    bool hasCustomScreen = false;
    bool trackCompatible = true;
    bool effectCompatible = false;
};

struct BundledPreset
{
    std::string key;
    std::string name;
};

struct ParamMeta
{
    std::uint32_t id = 0;
    std::string name{};
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool stepped = false;
    bool readOnly = false;
};

struct KnobReadout
{
    bool active = false;
    bool stepped = false;
    bool bipolar = false;
    std::uint32_t paramId = 0;
    float norm = 0.0f;
    char label[24]{};
    char valueText[24]{};
};

struct ScreenBitmap
{
    const std::uint32_t* argb = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint32_t serial = 0;
};

// Scans a Plugins/ directory for loadable *.clap bundles/binaries.
std::vector<CatalogueEntry> scanPluginDirectory(const std::string& pluginsDir, double sampleRate = 44100.0);

// One live CLAP plugin instance implementing core::audio::PluginAudioNode.
// Created via ClapAudioNode::create(...) on the message thread; destroyed on the message thread when
// the last intrusive atomic reference drops (both UI service and Engine RetireQueue have released it).
class ClapAudioNode final : public core::audio::PluginAudioNode
{
public:
    static ClapAudioNode* create(const std::string& absPath, const std::string& pluginId, double sampleRate,
                                 std::string& errorOut, bool effect = false, bool stateOnly = false,
                                 std::string_view initialParams = {});
    ~ClapAudioNode() override;

    ClapAudioNode(const ClapAudioNode&) = delete;
    ClapAudioNode& operator=(const ClapAudioNode&) = delete;

    // ---- audio thread (strictly RT-safe) ----
    void noteOn(int frame, int note, float velocity) noexcept override;
    void noteOff(int frame, int note) noexcept override;
    void allNotesOff(int frame) noexcept override;
    bool process(const core::audio::PluginProcessContext& ctx, float* outL, float* outR) noexcept override;
    void resetEffect() noexcept override;
    int effectLatency() const noexcept override { return latencySamples_; }
    void processEffect(const core::audio::PluginProcessContext& ctx, float* left, float* right) noexcept override;
    bool active() const noexcept override;

    // ---- message thread ----
    const std::string& id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const std::string& vendor() const noexcept { return vendor_; }
    const std::string& absPath() const noexcept { return absPath_; }
    double sampleRate() const noexcept { return sampleRate_; }

    bool bypassed() const noexcept { return bypassed_.load(std::memory_order_acquire); }
    void setBypassed(bool on) noexcept;

    std::size_t paramCount() const noexcept { return paramsMeta_.size(); }
    const ParamMeta* paramById(std::uint32_t id) const noexcept;
    const ParamMeta* paramByIndex(std::size_t idx) const noexcept;
    double paramValue(std::uint32_t id) const noexcept;
    bool formatParamValue(std::uint32_t id, double value, char* buf, std::size_t len) const noexcept;
    bool setParamValue(std::uint32_t id, double value) noexcept;

    int pageCount() const noexcept;
    int currentPage() const noexcept { return page_; }
    void setPage(int page) noexcept;

    bool readKnobs(std::array<KnobReadout, 6>& out, char outTab[SIXTEEN_TAB_NAME_SIZE]) const noexcept;
    bool turnKnob(int col0, int detents, bool fine, ClapAudioNode* stateEditor = nullptr) noexcept;
    void beforeScreenInput();
    void afterScreenInput();
    std::string editorState(ClapAudioNode* stateEditor) const;
    bool parameterEditPending() const noexcept
    {
        return paramHead_.load(std::memory_order_acquire) != paramTail_.load(std::memory_order_acquire);
    }
    bool pushKnob(int col0) noexcept;

    bool hasCustomScreen() const noexcept { return screenExt_ != nullptr; }
    ScreenBitmap renderScreen(std::uint32_t nowMs) noexcept;
    bool touchScreen(int phase, int x, int y) noexcept;
    bool selectorTurn(int delta) noexcept;
    bool selectorPush() noexcept;

    const std::vector<BundledPreset>& bundledPresets();
    bool loadBundledPreset(const std::string& key);
    std::string saveStateBase64() const;
    bool loadStateBase64(std::string_view b64);

    void pollMainThread() noexcept;

private:
    ClapAudioNode() = default;
    bool processBlock(const core::audio::PluginProcessContext&, float* outL, float* outR, float* inL,
                      float* inR) noexcept;
    int latencySamples_ = 0;
    std::uint32_t inputChannels_ = 2, outputChannels_ = 2;

    struct NoteEv
    {
        std::int32_t frame = 0;
        std::uint8_t kind = 0; // 1 = on, 2 = off, 3 = all-notes-off
        std::uint8_t note = 60;
        float velocity = 1.0f;
    };
    struct ParamEv
    {
        std::uint32_t id = 0;
        double value = 0.0;
    };

    static constexpr std::size_t kMaxNoteEv = 64;
    static constexpr std::size_t kParamQueueSize = 256; // power of two

    bool pushParamChange(std::uint32_t id, double value) noexcept;
    void refreshParamMeta();
    void flushInactiveParam(std::uint32_t id, double value);
    struct UiParamEdit
    {
        std::uint32_t id;
        double target, beforeInput;
    };
    std::vector<UiParamEdit> uiParamEdits_; // message thread only

    // CLAP host callbacks
    static const void* hostGetExtension(const clap_host* host, const char* extId) noexcept;
    static void hostRequestRestart(const clap_host* host) noexcept;
    static void hostRequestProcess(const clap_host* host) noexcept;
    static void hostRequestCallback(const clap_host* host) noexcept;

    void* dlHandle_ = nullptr;
    const clap_plugin_entry* entry_ = nullptr;
    const clap_plugin* plugin_ = nullptr;
    const clap_plugin_params* paramsExt_ = nullptr;
    const clap_plugin_state* stateExt_ = nullptr;
    const clap_plugin_preset_load* presetLoad_ = nullptr;
    std::optional<std::vector<BundledPreset>> presets_;
    const sixteen_plugin_screen_t* screenExt_ = nullptr;
    clap_host* hostStorage_ = nullptr; // heap-allocated clap_host_t with host_data = this
    bool activated_ = false;
    bool processing_ = false;
    double sampleRate_ = 44100.0;

    std::string absPath_;
    std::string id_;
    std::string name_;
    std::string vendor_;
    std::vector<ParamMeta> paramsMeta_;
    std::vector<double> cachedValues_;
    int page_ = 0;

    // Audio-thread state
    std::array<NoteEv, kMaxNoteEv> noteEvs_{};
    int noteEvCount_ = 0;
    int heldNotes_ = 0;
    int silentBlocks_ = 0;
    std::atomic<bool> bypassed_{false};
    std::atomic<bool> sleeping_{false};
    std::atomic<bool> wakeAudio_{true};
    std::atomic<bool> callbackRequested_{false};
    std::atomic<bool> flushRequested_{false};

    // Lock-free SPSC message -> audio parameter queue
    std::array<ParamEv, kParamQueueSize> paramQueue_{};
    std::atomic<std::size_t> paramHead_{0};
    std::atomic<std::size_t> paramTail_{0};

    // Audio scratch buffers (zeroed input for synths that read audio_inputs)
    alignas(16) std::array<float, engine::kBlockFrames> silenceL_{};
    alignas(16) std::array<float, engine::kBlockFrames> silenceR_{};

    // Custom body screen surface (allocated once if screenExt_ != nullptr)
    void openScreen() const noexcept;
    mutable bool screenOpen_ = false;
    std::unique_ptr<std::uint32_t[]> screenBuf_;
    std::uint32_t screenSerial_ = 0;
    bool screenInitialized_ = false;
};

// Manages mounted plugins across all 16 scenes x 16 tracks and swaps them into EngineBridge.
class ClapHostService
{
public:
    ClapHostService(engine::EngineBridge& bridge, std::string pluginsDir, double sampleRate,
                    std::function<void(const std::string&)> log = {});
    ~ClapHostService();

    ClapHostService(const ClapHostService&) = delete;
    ClapHostService& operator=(const ClapHostService&) = delete;

    const std::string& pluginsDir() const noexcept { return pluginsDir_; }
    void rescan();
    const std::vector<CatalogueEntry>& catalogue() const noexcept { return catalogue_; }

    // Scene and track are 0-based (0..15).
    bool mount(int scene0, int track0, const core::model::PluginTrack& spec);
    ClapAudioNode* mountedEffect(int scene0, int slot) const noexcept;
    bool hasEffect(int scene0, int slot) const noexcept { return mountedEffect(scene0, slot) != nullptr; }
    bool mountEffect(int scene0, int slot, const core::model::PluginTrack& spec);
    void acknowledgeEffectEdit(int scene0, int slot, const std::string& state);
    bool recallPreset(int scene0, int track0, std::optional<int> effectSlot, const core::model::PluginTrack& spec);
    void unmount(int scene0, int track0);
    void unmountAll();
    // Once audio stops, the engine drains its own queued and installed references on destruction.
    void detachFromEngine() noexcept { bridge_ = nullptr; }

    ClapAudioNode* mounted(int scene0, int track0) const noexcept;
    const core::model::PluginTrack& mountedSpec(int scene0, int track0,
                                                std::optional<int> effectSlot = {}) const noexcept;

    void syncFromProject(const core::model::Project& project);
    void syncChanges(const core::ChangeSet& changes, const core::model::Project& project);
    void snapshotStates(core::model::Project& project) const;
    void pollMainThread();

    std::uint32_t changes() const noexcept { return changes_; }
    void bumpChanges() noexcept { ++changes_; }

private:
    struct PendingMount
    {
        std::unique_ptr<ClapAudioNode> node;
        core::model::PluginTrack spec;
    };
    struct Slot
    {
        std::unique_ptr<PendingMount> pending;
        bool pendingClear = false;
        ClapAudioNode* node = nullptr; // owned UI ref (release() on unmount)
        core::model::PluginTrack spec{};
    };
    static std::size_t idx(int scene0, int track0) noexcept
    {
        return static_cast<std::size_t>(scene0 & 15) * 16 + static_cast<std::size_t>(track0 & 15);
    }
    bool mountSlot(Slot&, engine::SwapSlot, const core::model::PluginTrack&, bool recall = false, bool fresh = false,
                   bool strictPreset = false);
    bool postMount(Slot&, engine::SwapSlot, PendingMount&);
    void clearSlot(Slot&, engine::SwapSlot);
    void syncTrack(const core::model::Project&, int scene, int track, bool fresh = false);
    void syncEffect(const core::model::Project&, int scene, int slot, bool fresh = false);
    std::string resolvePath(const std::string& path) const;

    engine::EngineBridge* bridge_;
    std::string pluginsDir_;
    double sampleRate_ = 44100.0;
    std::function<void(const std::string&)> log_;
    std::vector<CatalogueEntry> catalogue_;
    std::array<Slot, 256> slots_{};
    std::vector<Slot> effects_ = std::vector<Slot>(16 * engine::kPluginFxSlots);
    std::uint32_t changes_ = 0;
};
} // namespace services::plugin
