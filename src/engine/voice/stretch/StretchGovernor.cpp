#include "engine/voice/stretch/StretchGovernor.h"

#include "engine/fx/FxSlotBank.h"

namespace engine::voice::stretch
{
GovernorDecision govern(const GovernorInput& in, const GovernorConfig& cfg) noexcept
{
    GovernorDecision d;
    const int reverb = in.reverb > 0 ? 1 : 0;
    const int inserts = in.activeInserts > 0 ? in.activeInserts : 0;
    const int players = in.masterTempoVoices > 0 ? in.masterTempoVoices : 0;
    d.score = 8 * reverb + inserts + 2 * players;
    d.tier = d.score >= cfg.lowQualityScore ? 1 : 0;
    const double voice = cfg.voiceCycles[d.tier];
    d.predictedCycles = cfg.baseCycles + inserts * cfg.insertCycles + reverb * cfg.reverbCycles + players * voice;
    d.holdNew = d.predictedCycles + voice > cfg.deadlineCycles;
    return d;
}

GovernorInput governorInput(const fx::FxSlotBank& bank) noexcept
{
    GovernorInput in;
    for (int s = 0; s < fx::kInsertSlots; ++s)
        if (! bank.slot(s).idle())
            ++in.activeInserts;
    const auto& send = bank.slot(fx::kSendSlot);
    if (! send.idle())
    {
        const auto* d = send.descriptor(send.current());
        if (d != nullptr && d->id == "reverb")
            in.reverb = 1;
        else
            ++in.activeInserts;
    }
    if (! bank.slot(fx::kMasterSlot).idle())
        ++in.activeInserts;
    return in;
}
} // namespace engine::voice::stretch
