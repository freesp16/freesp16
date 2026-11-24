// A golden subject: the C++ code a golden case runs (a cases.json "subject" id). One subject per model adapter in
// tools/golden-gen/goldenlib/models/ (same parameters, same input/output channel order).
//
//   mixer.track_strip  TrackStripSubject: engine::mixer::Mixer — the track strips, the main/send bus
//                      sums and optionally the bus chain (research track_mix.py + bus_mix.py with the app's GainLaw).
//   fx.gain_test       GainTestSubject: GainTest (GainTest.h) through the engine::fx::EffectBase contract.
//   fx.shell           FxShellSubject: engine::fx::FxShell / FxProcessor with stub effects (models/fx_shell.py).
//   fx.compressor      CompressorSubject: the Compressor + slot-18 mix (models/compressor.py, app maths).
//   fx.ducker          DuckerSubject: the Ducker with the trigger host rule (models/ducker.py).
//   fx.effect          EffectSubject: a registered effect through FxShell, Reference profile (models/lofi.py,
//                      models/distortion.py via goldenlib/effectrun.py).
//   fx.oscillator      OscillatorSubject: engine::fx::ModOscillator (models/oscillator.py).
//   fx.mod             ModSubject: fx.effect plus the tempo and retrigger (models/chorus.py, flanger.py, phaser.py via
//                      goldenlib/modrun.py).
//   fx.delay           DelaySubject: delay / delayBus through FxShell in a slot kind, with a tempo feed
//                      (models/fx_delay.py, fx_delaybus.py via goldenlib/delayrun.py). The Reverb is the app's own
//                      design (no research model): tests/unit/fx/test_fx_reverb.cpp checks its behaviour.
//   routing.router     RoutingSubject.cpp: engine::mixer::OutputRouter (models/routing_router.py).
//   routing.headphones RoutingSubject.cpp: engine::mixer::HeadphoneRouter (models/routing_headphones.py).
//   routing.device     RoutingSubject.cpp: OutputRouterStage steps 12-13, trim + mute-all (models/routing_device.py).
//
// Adding an effect golden: a subject that builds the effect, applies the case's parameter events per block
// and calls process(); register it in Subjects.cpp. configure() runs off the audio path (may allocate); process() runs
// under core::rt::ScopedRealtimeThread (APP_RT_CHECKS aborts on allocation) and must be noexcept.
#pragma once

#include "tests/golden/harness/GoldenCase.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace golden
{
enum class Fault
{
    None,
    Gain, // a deliberately wrong gain (x (1 + 2^-10), about +0.0085 dB): must fail with a readable report
};

constexpr float kFaultGainScale = 1.0f + 1.0f / 1024.0f;

class Subject
{
public:
    virtual ~Subject() = default;
    // Parse the case parameters and allocate. Returns "" or a readable error.
    virtual std::string configure(const GoldenCase& c) = 0;
    // Returns false when the subject has no such fault.
    virtual bool injectFault(Fault f) noexcept { return f == Fault::None; }
    // One 64-frame block: in[i] = input channel i at this block, out[o] = output channel o (written).
    virtual void process(int block, const float* const* in, float* const* out) noexcept = 0;
};

std::unique_ptr<Subject> makeSubject(std::string_view id);
std::vector<std::string_view> subjectIds();

std::unique_ptr<Subject> makeTrackStripSubject();
std::unique_ptr<Subject> makeGainTestSubject();
std::unique_ptr<Subject> makeFxShellSubject();
std::unique_ptr<Subject> makeFilterSubject(); // fx.filter: MultiModeFilter (FiltSubject.cpp)
std::unique_ptr<Subject> makeEq2Subject();    // fx.eq2: ParametricEq (FiltSubject.cpp)
std::unique_ptr<Subject> makeCompressorSubject();
std::unique_ptr<Subject> makeDuckerSubject();
std::unique_ptr<Subject> makeEffectSubject();
std::unique_ptr<Subject> makeOscillatorSubject();
std::unique_ptr<Subject> makeModSubject();
std::unique_ptr<Subject> makeDelaySubject();
std::unique_ptr<Subject> makeRouterSubject();
std::unique_ptr<Subject> makeHeadphonesSubject();
std::unique_ptr<Subject> makeDeviceSubject();

// Runs the subject over the whole case (configure, then every block under ScopedRealtimeThread).
// Returns "" or a readable error; `actual` gets one channel per output.
std::string runSubject(const GoldenCase& c, Subject& s, Channels& actual);
} // namespace golden
