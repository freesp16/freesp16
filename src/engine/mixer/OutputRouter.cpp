#include "engine/mixer/OutputRouter.h"

#include "engine/mixer/OutputRouterKernels.h"

#include <cstring>

namespace engine::mixer
{
namespace
{
constexpr int kN = kBlockFrames;

// One block start of the research Selector (headphone_mix.Selector.process, F209/F212): while the selection differs
// from the one playing, fade the old one out; in the first block that starts with the fade-out over, switch and fade
// the new one in. Re-selecting the one playing during its fade-out: the app fades back in from the gain reached; the
// reference (quirk) leaves the gain at 0 until the next change. Returns true when it switched.
template <class T> bool stepSelector(T& current, const T& selected, routing::SelectorFade& f, bool quirk) noexcept
{
    if (! (current == selected))
    {
        f.setTarget(0.0f);
        if (! f.idle())
            return false;
        current = selected;
        f.setTarget(1.0f);
        return true;
    }
    if (! quirk && f.target() == 0.0f)
        f.setTarget(1.0f);
    return false;
}
} // namespace

OutputRouter::OutputRouter() noexcept { updateRouted(); }

void OutputRouter::prepare(double, std::span<const ChannelMapEntry> map) noexcept
{
    // Settle every fade on its target (the router starts at its parameters, like the mixer).
    mode_ = pendingMode_;
    modeFade_.reset(1.0f);
    for (auto& a : assign_)
    {
        a.current = a.pending;
        a.fade.reset(1.0f);
    }
    trackFades_ = 0;
    mutePos_ = kMuteAllFrames;
    muteRamping_ = false;
    muteSilent_ = muteAll_;
    if (map.empty())
        present_ = 0x0F; // no map = every pair (tests, the offline render)
    else
    {
        present_ = 1; // OUT 1-2 always (MIX-032: everything folds into it)
        for (const auto& e : map)
        {
            const int p = static_cast<int>(e.port);
            if (p > 0 && p < kRouterPairs && e.left >= 0 && e.right >= 0)
                present_ = static_cast<std::uint8_t>(present_ | 1u << p);
        }
    }
    tapL_ = tapR_ = nullptr;
    dirty_ = true;
}

void OutputRouter::setMode(AssignMode m) noexcept
{
    pendingMode_ = m; // the Selector's target; process() fades (research Output12Router 0x1b28f8 stores the selection)
}

void OutputRouter::setAssign(int track, OutAssign a) noexcept
{
    if (track < 0 || track >= kTracks)
        return;
    if (a.pair < 0 || a.pair >= kRouterPairs)
        a.pair = -1;
    if (a.pair >= 0 || a.mono < 0 || a.mono >= 2 * kRouterPairs)
        a.mono = -1;
    auto& s = assign_[static_cast<std::size_t>(track)];
    if (a == s.pending)
        return;
    s.pending = a;
    trackFades_ |= 1u << track; // process() steps its selector until it has settled
}

void OutputRouter::setPresent(std::uint8_t pairMask) noexcept
{
    const auto p = static_cast<std::uint8_t>((pairMask | 1u) & 0x0Fu);
    if (p == present_)
        return;
    present_ = p;
    dirty_ = true;
}

void OutputRouter::setMuteAll(bool on) noexcept
{
    if (on == muteAll_)
        return;
    muteAll_ = on;
    mutePos_ = 0; // one 64-frame block per fade: the fade always starts on a block boundary
}

bool OutputRouter::resolve(OutAssign a, int& pair, int& mono) const noexcept
{
    pair = mono = -1;
    if (mode_ == AssignMode::MasterPlus6)
    {
        // OUT 1-2 and the mono channels 1/2 are not selectable in M + 6CH (M p.23): master only.
        if (a.pair >= 1)
            pair = a.pair;
        else if (a.mono >= 2)
            mono = a.mono;
        else
            return false;
        const int p = pair >= 0 ? pair : mono >> 1;
        return ((present_ >> p) & 1u) != 0; // an absent pair folds into Main, where the master already has the track
    }
    // 8CH: "--" = OUT 1-2 (MIX-031); an absent pair folds into OUT 1-2 (a mono channel keeps its side).
    if (a.mono >= 0)
    {
        mono = ((present_ >> (a.mono >> 1)) & 1u) != 0 ? a.mono : (a.mono & 1);
        return true;
    }
    pair = a.pair >= 0 && ((present_ >> a.pair) & 1u) != 0 ? a.pair : 0;
    return true;
}

void OutputRouter::updateRouted() noexcept
{
    routed_ = 0;
    for (int t = 0; t < kTracks; ++t)
    {
        int p = 0, m = 0;
        if (resolve(assign_[static_cast<std::size_t>(t)].current, p, m))
            routed_ |= 1u << t;
    }
    dirty_ = false;
}

float* OutputRouter::pairFor(int pair) noexcept
{
    float* l = pairBuf_[pair][0];
    if (((used_ >> pair) & 1u) == 0)
    {
        std::memset(pairBuf_[pair], 0, sizeof pairBuf_[pair]);
        used_ = static_cast<std::uint8_t>(used_ | 1u << pair);
    }
    return l;
}

void OutputRouter::addTrack(int track, const stages::StereoBlock& b, const float* gain) noexcept
{
    int pair = -1, mono = -1;
    if (! resolve(assign_[static_cast<std::size_t>(track)].current, pair, mono))
        return;
    if (pair >= 0)
    {
        float* l = pairFor(pair);
        float* r = pairBuf_[pair][1];
        if (gain == nullptr)
        {
            routing::add(l, b.l, kN);
            routing::add(r, b.r, kN);
        }
        else
        {
            routing::addGain(l, b.l, gain, kN);
            routing::addGain(r, b.r, gain, kN);
        }
        return;
    }
    pairFor(mono >> 1);
    float* dst = pairBuf_[mono >> 1][mono & 1];
    if (gain == nullptr)
        routing::addMono(dst, b.l, b.r, kN);
    else
        routing::addMonoGain(dst, b.l, b.r, gain, kN);
}

void OutputRouter::stepSelectors() noexcept
{
    if (stepSelector(mode_, pendingMode_, modeFade_, quirk_))
        dirty_ = true; // MIX-033: the switch, between the fade-out and the fade-in
    for (std::uint32_t f = trackFades_, t = 0; f != 0; ++t, f >>= 1)
        if ((f & 1u) != 0 && stepSelector(assign_[t].current, assign_[t].pending, assign_[t].fade, false))
            dirty_ = true;
}

void OutputRouter::process(const TrackOutputs& tracks, const float* const* master, OutputBuffers& out) noexcept
{
    tapL_ = master != nullptr ? master[0] : nullptr;
    tapR_ = master != nullptr ? master[1] : nullptr;
    out.ch.fill(nullptr);
    used_ = 0;
    stepSelectors();
    if (dirty_)
        updateRouted();

    // The mode fade of this block (the selector's gains are applied in the block that computes them).
    const bool modeFading = ! modeFade_.idle();
    const bool modeSilent = ! modeFading && modeFade_.current() == 0.0f; // the reference quirk's stuck state
    if (modeFading)
        modeFade_.fill(modeGain_, kN);

    // The assign feeds: tracks with a direct output and a post-fader block this block; every changing or fading track
    // steps its fade (also while silent, as the reference selectors run every block).
    const bool haveTracks = tracks.track != nullptr;
    std::uint32_t todo = (routed_ & tracks.valid) | trackFades_;
    for (int t = 0; todo != 0; ++t, todo >>= 1)
    {
        if ((todo & 1u) == 0)
            continue;
        auto& s = assign_[static_cast<std::size_t>(t)];
        const bool feed = haveTracks && ((tracks.valid >> t) & 1u) != 0 && ((routed_ >> t) & 1u) != 0;
        const float* g = nullptr;
        if ((trackFades_ >> t) & 1u)
        {
            if (! s.fade.idle())
            {
                s.fade.fill(feed ? trackGain_ : nullptr, kN);
                g = trackGain_;
            }
            if (s.current == s.pending && s.fade.idle() && s.fade.current() == 1.0f)
                trackFades_ &= ~(1u << t); // settled (the gains of this block are still applied below)
        }
        if (feed)
            addTrack(t, tracks.track[t], g);
    }

    // OUT 1-2 in M + 6CH: the master itself (no copy unless the mode fade runs; no track can be routed to it).
    const bool mainIsMaster = mode_ == AssignMode::MasterPlus6 && tapL_ != nullptr;
    if (modeSilent)
    {
        renderMuteAll();
        return; // every router output silent until the next mode change (reference profile only)
    }
    if (mainIsMaster)
    {
        if (modeFading)
        {
            routing::copyGain(pairBuf_[0][0], tapL_, modeGain_, kN);
            routing::copyGain(pairBuf_[0][1], tapR_, modeGain_, kN);
            out.ch[0] = pairBuf_[0][0];
            out.ch[1] = pairBuf_[0][1];
        }
        else
        {
            out.ch[0] = tapL_;
            out.ch[1] = tapR_;
        }
    }
    for (int p = 0; p < kRouterPairs; ++p)
    {
        if (((used_ >> p) & 1u) == 0 || (p == 0 && mainIsMaster))
            continue;
        if (modeFading)
        {
            routing::multiply(pairBuf_[p][0], modeGain_, kN);
            routing::multiply(pairBuf_[p][1], modeGain_, kN);
        }
        out.ch[static_cast<std::size_t>(2 * p)] = pairBuf_[p][0];
        out.ch[static_cast<std::size_t>(2 * p + 1)] = pairBuf_[p][1];
    }
    renderMuteAll();
}

void OutputRouter::renderMuteAll() noexcept
{
    if (mutePos_ >= kMuteAllFrames)
    {
        muteRamping_ = false;
        muteSilent_ = muteAll_;
        return;
    }
    constexpr float kInv = 1.0f / static_cast<float>(kMuteAllFrames);
    for (int i = 0; i < kN; ++i)
    {
        const int k = mutePos_ + i;
        // The research BulkMute's MuteSmoother(64) (device_out.py, F109–F113): 1 … 1/64 then 0; 0 … 63/64 then 1.
        muteGain_[i] = muteAll_ ? (k < kMuteAllFrames ? static_cast<float>(kMuteAllFrames - k) * kInv : 0.0f)
                                : (k < kMuteAllFrames ? static_cast<float>(k) * kInv : 1.0f);
    }
    mutePos_ += kN;
    muteRamping_ = true;
    muteSilent_ = false;
}

std::uint8_t OutputRouter::destinations(int track) const noexcept
{
    int pair = -1, mono = -1;
    if (! resolve(assign_[static_cast<std::size_t>(track & (kTracks - 1))].current, pair, mono))
        return 0;
    if (pair >= 0)
        return static_cast<std::uint8_t>((1u << pair) | (1u << (4 + pair)));
    return static_cast<std::uint8_t>(1u << ((mono >> 1) + ((mono & 1) != 0 ? 4 : 0)));
}
} // namespace engine::mixer
