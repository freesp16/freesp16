// Everything of steps 6 and 8a that engine::Engine owns: the trigger router (pad repeat is the
// strip's, engine/ribbon/RibbonEngine), the 16 track sources on one AmpEnvelopeBank, their [scene][track] parameter blocks (envelope, playback;
// a scene switch is an index change, never a copy) and the BROWSE preview voice. One plain member of the
// engine with an out-of-line constructor and destructor: GCC 11 at -O2/-O3 expands every default member initialiser of
// a constructor inline, and engine::Engine's constructor (SPSC queue of 4096 messages, swap and binding tables) is
// already the slowest function of the device build — this keeps the ≈ 20 k element initialisations here in their own
// translation unit.
#pragma once

#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/modulation/AmpEnvelopeParams.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerRouter.h"
#include "engine/voice/PlaybackParams.h"
#include "engine/voice/PreviewVoice.h"

#include <array>

namespace engine::trigger
{
struct SourceSet
{
    SourceSet() noexcept;
    ~SourceSet();
    SourceSet(const SourceSet&) = delete;
    SourceSet& operator= (const SourceSet&) = delete;

    TriggerRouter router;
    std::array<TrackSource, kTracks> sources;
    modulation::AmpEnvelopeBank env;
    modulation::AmpEnvParams envParams[16][kTracks]; // [scene][track], default-initialised (no braces)
    voice::PlaybackParamBlock playback;
    voice::PreviewVoice preview; // BROWSE preview → aux (post-master, LIB-011)
};
} // namespace engine::trigger
