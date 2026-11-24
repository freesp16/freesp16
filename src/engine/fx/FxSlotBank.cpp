// FxSlotBank — see FxSlotBank.h.
#include "engine/fx/FxSlotBank.h"
#include "engine/fx/effects/Plugin.h"

#include "engine/fx/EffectRegistryOrder.h"

#include <cstring>

namespace engine::fx
{
namespace
{
float clamp01(float v) noexcept
{
    return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f; // NaN → 0 (FX-057)
}
int roundInt(float v) noexcept
{
    return static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}
SlotKind kindOf(int s) noexcept
{
    return s < kInsertSlots ? SlotKind::Insert : (s == kSendSlot ? SlotKind::Send : SlotKind::Master);
}
} // namespace

FxSlotBank::FxSlotBank()
{
    for (auto& s : slots_)
        s = std::make_unique<FxProcessor>(); // rt-ok: message thread, construction
    for (int i = 0; i < kTracks; ++i)
        hooks_[static_cast<std::size_t>(i)] = Hook{};
    for (int i = 0; i < kTracks + 2; ++i)
    {
        hooks_[static_cast<std::size_t>(i)].bank = this;
        hooks_[static_cast<std::size_t>(i)].which = i < kTracks ? i : (i == kTracks ? kSendSlot : kMasterSlot);
    }
    ctx_.triggerOffset.fill(-1);
}

void FxSlotBank::Hook::process(float* l, float* r) noexcept
{
    if (which < kTracks)
        bank->processInsert(which, l, r, bank->ctx_);
    else if (which == kSendSlot)
    {
        if (! bank->processSend(l, r, bank->ctx_)) // idle between fillHooks and now: keep the contract (silence)
        {
            std::memset(l, 0, sizeof(float) * kBlockFrames);
            std::memset(r, 0, sizeof(float) * kBlockFrames);
        }
    }
    else
        bank->processMaster(l, r, bank->ctx_);
}

void FxSlotBank::fillHooks(mixer::Hooks& h) noexcept
{
    for (int t = 0; t < kTracks; ++t)
        h.insert[static_cast<std::size_t>(t)] = insertIdle(t) ? nullptr : &hooks_[static_cast<std::size_t>(t)];
    h.sendFx = sendIdle() ? nullptr : &hooks_[kTracks];
    h.masterFx = masterIdle() ? nullptr : &hooks_[kTracks + 1];
}

std::uint32_t FxSlotBank::claimTailTracks(stages::BlockBuffers& b) noexcept
{
    std::uint32_t claimed = 0;
    for (int t = 0; t < kTracks; ++t)
        if (((b.activeTracks >> t) & 1u) == 0 && ! insertIdle(t))
        {
            b.track[static_cast<std::size_t>(t)].clear();
            claimed |= 1u << t;
        }
    return claimed;
}

FxSlotBank::~FxSlotBank() = default;

void FxSlotBank::build(const EffectRegistry& registry, std::span<const std::string_view> paramIds,
                       std::span<const std::string_view> enabledFlags, std::span<const std::int8_t> internalIndex)
{
    insertTypes_ = listFor(registry, SlotKind::Insert, enabledFlags);
    sendTypes_ = listFor(registry, SlotKind::Send, enabledFlags);
    masterTypes_ = listFor(registry, SlotKind::Master, enabledFlags);
    for (int s = 0; s < kFxSlots; ++s)
    {
        const SlotKind k = kindOf(s);
        const auto& list = k == SlotKind::Insert ? insertTypes_ : (k == SlotKind::Send ? sendTypes_ : masterTypes_);
        slots_[static_cast<std::size_t>(s)]->build(k, list);
        auto* plugin = dynamic_cast<effects::Plugin*>(slots_[s]->effect(typeIndexOf(list, "plugin")));
        plugins_[s] = plugin;
        if (plugin)
            plugin->send = k == SlotKind::Send;
        auto& st = state_[static_cast<std::size_t>(s)];
        st = SlotParams{};
        st.stored.assign(list.size() * 6, 0.0f); // rt-ok: message thread, build()
    }
    routes_.build(insertTypes_, sendTypes_, masterTypes_, paramIds, internalIndex);
}

void FxSlotBank::setPlugin(int slot, core::audio::PluginAudioNode* node) noexcept
{
    if (slot >= 0 && slot < kFxSlots && plugins_[slot])
        plugins_[slot]->node = node;
}

bool FxSlotBank::prepare(double sampleRate, int maxBlock)
{
    for (auto& s : slots_)
        if (! s->prepare(sampleRate, maxBlock))
            return false;
    const auto f = flagsFor(mode_);
    slots_[kSendSlot]->setDisabled(f.sendDisabled);
    slots_[kMasterSlot]->setDisabled(f.masterDisabled);
    midiMask_ = 0;
    if (metered_ >= 0)
        slots_[static_cast<std::size_t>(metered_)]->setMetered(true);
    return true;
}

float FxSlotBank::stored(int s, int type, int k) const noexcept
{
    const auto& st = state_[static_cast<std::size_t>(s)];
    const auto i = static_cast<std::size_t>(type * 6 + k);
    return type >= 0 && i < st.stored.size() ? st.stored[i] : 0.0f;
}

int FxSlotBank::slotOf(const FxRoute& r, std::uint16_t scope, bool& ok) const noexcept
{
    ok = true;
    switch (r.slotKind)
    {
    case SlotKind::Insert:
        if ((scope >> 4) != scene_)
            break;
        return insertSlot(scope & 15, r.unit);
    case SlotKind::Send:
        if (scope != scene_)
            break;
        return kSendSlot;
    case SlotKind::Master:
        if (scope != scene_)
            break;
        return kMasterSlot;
    }
    ok = false;
    return 0;
}

void FxSlotBank::push(int s, int type, int k) noexcept
{
    const auto& st = state_[static_cast<std::size_t>(s)];
    float v = st.effective[k];
    if (((st.modMask >> k) & 1u) != 0 && st.modType == type)
        v = clamp01(v + st.amount[k]); // FX-084
    slots_[static_cast<std::size_t>(s)]->setParam(type, k, v);
}

void FxSlotBank::retype(int s, int type) noexcept
{
    auto& st = state_[static_cast<std::size_t>(s)];
    st.lockMask = 0;
    if (type < 0)
        return;
    for (int k = 0; k < 6; ++k)
    {
        st.effective[k] = st.stored[static_cast<std::size_t>(type * 6 + k)];
        push(s, type, k);
    }
}

void FxSlotBank::onParamChange(const ParamChange& c) noexcept
{
    const FxRoute& r = routes_.find(c.param);
    switch (r.kind)
    {
    case FxRoute::Kind::None:
        return;
    case FxRoute::Kind::Mode:
        if (c.scope == scene_)
            setFxMode(static_cast<std::uint8_t>(roundInt(c.value)));
        return;
    case FxRoute::Kind::Quirk:
        setQuirkProfile(c.value >= 0.5f ? kQuirkProfileReference : kQuirkProfileSixteen);
        return;
    case FxRoute::Kind::Meter:
        setMeteredSlot(roundInt(c.value));
        return;
    default:
        break;
    }
    bool ok = false;
    const int s = slotOf(r, c.scope, ok);
    if (! ok)
        return;
    auto& proc = *slots_[static_cast<std::size_t>(s)];
    auto& st = state_[static_cast<std::size_t>(s)];
    switch (r.kind)
    {
    case FxRoute::Kind::SlotType:
    {
        const int before = proc.pending();
        proc.requestType(roundInt(c.value) - 1);
        if (proc.pending() != before)
            retype(s, proc.pending());
        break;
    }
    case FxRoute::Kind::SlotBypass:
        proc.setBypass(c.value >= 0.5f);
        break;
    case FxRoute::Kind::Knob:
    {
        const int type = r.type, k = r.index;
        const float v = clamp01(c.value);
        st.stored[static_cast<std::size_t>(type * 6 + k)] = v;
        if (type != proc.pending())
            proc.setParam(type, k, v);           // the type's own instance keeps its last value (FX-006)
        else if (((st.lockMask >> k) & 1u) == 0) // FX-081: an edit during a lock changes only the stored value
        {
            st.effective[k] = v;
            push(s, type, k);
        }
        break;
    }
    case FxRoute::Kind::Option:
        proc.setOption(r.type, r.index, roundInt(c.value));
        break;
    default:
        break;
    }
}

void FxSlotBank::applyScene(const FxSlotState& scene) noexcept
{
    scene_ = scene.scene;
    for (int s = 0; s < kFxSlots; ++s)
    {
        const auto& in = scene.slot[static_cast<std::size_t>(s)];
        auto& proc = *slots_[static_cast<std::size_t>(s)];
        auto& st = state_[static_cast<std::size_t>(s)];
        proc.requestType(in.type);
        proc.setBypass(in.bypass != 0);
        const int t = proc.pending();
        if (t < 0)
        {
            st.lockMask = 0;
            continue;
        }
        for (int k = 0; k < 6; ++k)
            st.stored[static_cast<std::size_t>(t * 6 + k)] = clamp01(in.params[k]);
        for (int i = 0; i < kSlotOptions; ++i)
            if (((in.optionMask >> i) & 1u) != 0)
                proc.setOption(t, i, in.options[i]);
        retype(s, t);
    }
    setFxMode(scene.fxMode);
    setQuirkProfile(scene.quirkMask);
}

void FxSlotBank::applyStepLocks(int track, const FxStepLocks& locks) noexcept
{
    for (int u = 0; u < 2; ++u)
    {
        const int s = insertSlot(track, u);
        const int t = slots_[static_cast<std::size_t>(s)]->pending();
        auto& st = state_[static_cast<std::size_t>(s)];
        if (t < 0)
        {
            st.lockMask = 0;
            continue;
        }
        const std::uint8_t mask = locks.type[u] == t ? locks.mask[u] : 0; // FX-080
        for (int k = 0; k < 6; ++k)
        {
            const bool locked = ((mask >> k) & 1u) != 0;
            const float v = locked ? clamp01(locks.value[u][k]) : st.stored[static_cast<std::size_t>(t * 6 + k)];
            if (v != st.effective[k])
            {
                st.effective[k] = v;
                push(s, t, k);
            }
        }
        st.lockMask = mask;
    }
}

void FxSlotBank::setModulation(int track, int unit, int type, int k, float amount) noexcept
{
    if (track < 0 || track >= kTracks || unit < 0 || unit > 1 || k < 0 || k >= 6)
        return;
    const int s = insertSlot(track, unit);
    auto& st = state_[static_cast<std::size_t>(s)];
    if (st.modType != type) // the destination moved to another type: the old one's values return unmodulated
    {
        const std::uint8_t old = st.modMask;
        const int oldType = st.modType;
        st.modMask = 0;
        st.modType = static_cast<std::int8_t>(type);
        if (oldType >= 0 && oldType == slots_[static_cast<std::size_t>(s)]->pending())
            for (int j = 0; j < 6; ++j)
                if (((old >> j) & 1u) != 0)
                    push(s, oldType, j);
    }
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << k);
    const bool was = (st.modMask & bit) != 0;
    if (amount == 0.0f)
        st.modMask = static_cast<std::uint8_t>(st.modMask & ~bit);
    else
        st.modMask = static_cast<std::uint8_t>(st.modMask | bit);
    st.amount[k] = amount;
    if (type == slots_[static_cast<std::size_t>(s)]->pending() && (was || amount != 0.0f))
        push(s, type, k); // FX-085: another type's destination does nothing
}

void FxSlotBank::setFxMode(std::uint8_t m) noexcept
{
    mode_ = fxModeFromValue(m);
    const auto f = flagsFor(mode_);
    slots_[kSendSlot]->setDisabled(f.sendDisabled);
    slots_[kMasterSlot]->setDisabled(f.masterDisabled);
}

void FxSlotBank::setQuirkProfile(std::uint32_t mask) noexcept
{
    for (auto& s : slots_)
        s->setQuirkProfile(mask);
}

void FxSlotBank::setMeteredSlot(int slot) noexcept
{
    if (metered_ >= 0)
        slots_[static_cast<std::size_t>(metered_)]->setMetered(false);
    metered_ = slot >= 0 && slot < kFxSlots ? slot : -1;
    if (metered_ >= 0)
        slots_[static_cast<std::size_t>(metered_)]->setMetered(true);
}

void FxSlotBank::onTransportStart() noexcept
{
    if (! restartLfos_)
        return;
    for (auto& s : slots_)
        s->retrigger();
}

void FxSlotBank::silenceAndClear() noexcept
{
    for (auto& s : slots_)
        s->silenceAndClear();
}

ProcessContext FxSlotBank::beginBlock(const BlockContext& ctx, std::uint16_t midiTrackMask) noexcept
{
    ++blockIndex_;
    triggerFlash_ = ctx.triggerMask;
    if (ctx.startedThisBlock)
        onTransportStart();
    if (midiTrackMask != midiMask_) // FX-002: both inserts of a MIDI track are disabled
    {
        const std::uint16_t changed = static_cast<std::uint16_t>(midiTrackMask ^ midiMask_);
        for (int t = 0; t < kTracks; ++t)
            if (((changed >> t) & 1u) != 0)
            {
                const bool midi = ((midiTrackMask >> t) & 1u) != 0;
                slots_[static_cast<std::size_t>(2 * t)]->setDisabled(midi);
                slots_[static_cast<std::size_t>(2 * t + 1)]->setDisabled(midi);
            }
        midiMask_ = midiTrackMask;
    }
    ctx_ = ProcessContext{ctx.sampleRate,    ctx.bpm,       ctx.playing, ctx.triggerMask,
                          ctx.triggerOffset, midiTrackMask, ctx.ppqStart};
    return ctx_;
}

void FxSlotBank::publish(FxMeterFeed& f) const noexcept
{
    f.blockIndex = blockIndex_;
    f.meteredSlot = static_cast<std::int8_t>(metered_);
    f.triggerFlash = triggerFlash_;
    f.runningMask = 0;
    for (int s = 0; s < kFxSlots; ++s)
    {
        const auto& p = *slots_[static_cast<std::size_t>(s)];
        f.current[static_cast<std::size_t>(s)] = static_cast<std::int8_t>(p.current());
        if (! p.idle())
            f.runningMask |= std::uint64_t{1} << s;
    }
    if (metered_ >= 0)
    {
        const auto& p = *slots_[static_cast<std::size_t>(metered_)];
        f.inPeak = p.inPeak();
        f.outPeak = p.outPeak();
        f.telemetry[0] = p.telemetry(0);
        f.telemetry[1] = p.telemetry(1);
    }
    else
        f.inPeak = f.outPeak = f.telemetry[0] = f.telemetry[1] = 0.0f;
}
} // namespace engine::fx
