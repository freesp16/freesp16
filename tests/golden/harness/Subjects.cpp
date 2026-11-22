#include "tests/golden/harness/Subject.h"

#include "core/util/Realtime.h"
#include "engine/EngineApi.h"

namespace golden
{
namespace
{
struct Entry
{
    std::string_view id;
    std::unique_ptr<Subject> (*make)();
};
constexpr Entry kSubjects[] = {
    {"mixer.track_strip", &makeTrackStripSubject},
    {"fx.gain_test", &makeGainTestSubject},
    {"fx.shell", &makeFxShellSubject},
    {"fx.filter", &makeFilterSubject},
    {"fx.eq2", &makeEq2Subject},
    {"fx.compressor", &makeCompressorSubject},
    {"fx.ducker", &makeDuckerSubject},
    {"fx.effect", &makeEffectSubject},
    {"fx.oscillator", &makeOscillatorSubject},
    {"fx.mod", &makeModSubject},
    {"fx.delay", &makeDelaySubject},
    {"routing.router", &makeRouterSubject},
    {"routing.headphones", &makeHeadphonesSubject},
    {"routing.device", &makeDeviceSubject},
};
} // namespace

std::unique_ptr<Subject> makeSubject(std::string_view id)
{
    for (const auto& e : kSubjects)
        if (e.id == id)
            return e.make();
    return nullptr;
}

std::vector<std::string_view> subjectIds()
{
    std::vector<std::string_view> ids;
    for (const auto& e : kSubjects)
        ids.push_back(e.id);
    return ids;
}

std::string runSubject(const GoldenCase& c, Subject& s, Channels& actual)
{
    if (c.block != engine::kBlockFrames)
        return "the golden's block is not the engine block (64)";
    if (auto e = s.configure(c); ! e.empty())
        return c.label() + ": " + e;
    actual.assign(c.outputNames.size(), std::vector<float>(static_cast<std::size_t>(c.frames), 0.0f));
    std::vector<const float*> in(c.input.size());
    std::vector<float*> out(actual.size());
    {
        core::rt::ScopedRealtimeThread rt; // APP_RT_CHECKS: an allocation in process() aborts the run
        for (int b = 0; b < c.blocks; ++b)
        {
            const auto off = static_cast<std::size_t>(b * c.block);
            for (std::size_t i = 0; i < in.size(); ++i)
                in[i] = c.input[i].data() + off;
            for (std::size_t o = 0; o < out.size(); ++o)
                out[o] = actual[o].data() + off;
            s.process(b, in.data(), out.data());
        }
    }
    return {};
}
} // namespace golden
