// Listening renders for a blind listening test of the time-stretch backends. Hidden — never run by ctest; run it
// by hand from a Release build:
//
//   build/host-fast/tests/<prefix>_bench "[.listening]"        → build/host-fast/stretch-listening/
//
// Four generated 2-bar loops at 120 BPM (drum loop, vowel "vocal", pad, bass; no recorded material, nothing
// firmware-derived) through every backend × tier — the project tempo at ±10 % and ±25 %, and pitch ±7 st at the
// sample's tempo — written as 16-bit stereo WAVs named <stimulus>_<case>_<letter>.wav. The letters are shuffled per
// stimulus and case (fixed seed), so the files can be compared blind; key.csv maps the letters to the backends, and
// <stimulus>_original.wav is the input. "none" (RESAMPLE: the pitch follows the tempo) is the reference for the tempo
// cases only.
#include "engine/voice/TimeStretcher.h"
#include "engine/voice/stretch/GrainStretcher.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr double kRate = 44100.0;
constexpr int kFrames = 4 * 44100; // 2 bars at 120 BPM

struct Stereo
{
    std::vector<float> l, r;
};

double env(double t, double attack, double decay)
{
    return t < 0 ? 0 : (t < attack ? t / attack : std::exp(-(t - attack) / decay));
}

Stereo drums()
{
    Stereo s {std::vector<float>(kFrames), std::vector<float>(kFrames)};
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float hp = 0.0f, prev = 0.0f;
    for (int i = 0; i < kFrames; ++i)
    {
        const double t = i / kRate, beat = std::fmod(t, 0.5), eighth = std::fmod(t, 0.25);
        const int beatIndex = static_cast<int>(t / 0.5);
        const double kick = env(beat, 0.002, 0.12) * std::sin(2 * M_PI * (50.0 * beat + 70.0 * 0.04 * (1 - std::exp(-beat / 0.04))));
        const float n = u(rng);
        const double snare = (beatIndex % 2 == 1) ? env(beat, 0.001, 0.08) * (0.6 * static_cast<double>(n) + 0.4 * std::sin(2 * M_PI * 185.0 * beat)) : 0.0;
        hp = 0.7f * (hp + n - prev); // a crude high-pass for the hats
        prev = n;
        const double hat = env(eighth, 0.0005, 0.025) * static_cast<double>(hp) * 0.35;
        s.l[static_cast<std::size_t>(i)] = static_cast<float>(0.8 * kick + 0.5 * snare + hat * 1.2);
        s.r[static_cast<std::size_t>(i)] = static_cast<float>(0.8 * kick + 0.5 * snare + hat * 0.6);
    }
    return s;
}

// Two-pole resonator (formant).
struct Reson
{
    double a1 = 0, a2 = 0, g = 0, y1 = 0, y2 = 0;
    void set(double f, double bw)
    {
        const double r = std::exp(-M_PI * bw / kRate);
        a1 = 2 * r * std::cos(2 * M_PI * f / kRate);
        a2 = -r * r;
        g = 1 - r;
    }
    double run(double x)
    {
        const double y = g * x + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

Stereo vocal()
{
    Stereo s {std::vector<float>(kFrames), std::vector<float>(kFrames)};
    const double vowels[4][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}}; // a i u e
    Reson f[3];
    double phase = 0;
    for (int i = 0; i < kFrames; ++i)
    {
        const double t = i / kRate;
        const int v = static_cast<int>(t / 0.5) % 4;
        if (i % 256 == 0)
            for (int k = 0; k < 3; ++k)
                f[k].set(vowels[v][k], 80.0 + 40.0 * k);
        const double hz = 220.0 * (1.0 + 0.012 * std::sin(2 * M_PI * 5.5 * t)) * (v % 2 ? 1.1225 : 1.0);
        phase += hz / kRate;
        phase -= std::floor(phase);
        const double saw = 2 * phase - 1;
        const double y = (f[0].run(saw) + 0.6 * f[1].run(saw) + 0.3 * f[2].run(saw)) * env(std::fmod(t, 0.5), 0.03, 0.6) * 2.5;
        s.l[static_cast<std::size_t>(i)] = s.r[static_cast<std::size_t>(i)] = static_cast<float>(y);
    }
    return s;
}

Stereo pad()
{
    Stereo s {std::vector<float>(kFrames), std::vector<float>(kFrames)};
    const double notes[3] = {220.0, 277.18, 329.63};
    double ph[3][3][2] = {};
    double lpL = 0, lpR = 0;
    for (int i = 0; i < kFrames; ++i)
    {
        const double t = i / kRate;
        double l = 0, r = 0;
        for (int n = 0; n < 3; ++n)
            for (int d = 0; d < 3; ++d)
                for (int c = 0; c < 2; ++c)
                {
                    const double det = 1.0 + (d - 1) * 0.004 + c * 0.0015;
                    double& p = ph[n][d][c];
                    p += notes[n] * det / kRate;
                    p -= std::floor(p);
                    (c == 0 ? l : r) += 2 * p - 1;
                }
        const double e = std::min(1.0, t / 0.8) * std::min(1.0, (4.0 - t) / 0.5);
        lpL += 0.08 * (l - lpL);
        lpR += 0.08 * (r - lpR);
        s.l[static_cast<std::size_t>(i)] = static_cast<float>(0.09 * e * lpL);
        s.r[static_cast<std::size_t>(i)] = static_cast<float>(0.09 * e * lpR);
    }
    return s;
}

Stereo bass()
{
    Stereo s {std::vector<float>(kFrames), std::vector<float>(kFrames)};
    const double riff[16] = {55, 55, 82.41, 55, 73.42, 55, 65.41, 73.42, 55, 55, 82.41, 98, 73.42, 65.41, 55, 49};
    double ph = 0;
    for (int i = 0; i < kFrames; ++i)
    {
        const double t = i / kRate, sixteenth = std::fmod(t, 0.125);
        const double hz = riff[static_cast<int>(t / 0.125) % 16];
        ph += hz / kRate;
        ph -= std::floor(ph);
        const double y = (0.7 * std::sin(2 * M_PI * ph) + 0.25 * (2 * ph - 1)) * env(sixteenth, 0.003, 0.09);
        s.l[static_cast<std::size_t>(i)] = s.r[static_cast<std::size_t>(i)] = static_cast<float>(0.8 * y);
    }
    return s;
}

Stereo normalised(Stereo s, float peak = 0.7f) // every stimulus at the same peak, well below clipping
{
    float m = 1.0e-9f;
    for (const auto* v : {&s.l, &s.r})
        for (const float x : *v)
            m = std::max(m, std::fabs(x));
    for (auto* v : {&s.l, &s.r})
        for (float& x : *v)
            x *= peak / m;
    return s;
}

void writeWav(const std::filesystem::path& p, const std::vector<float>& l, const std::vector<float>& r)
{
    std::ofstream f(p, std::ios::binary);
    const auto frames = static_cast<std::uint32_t>(l.size());
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + frames * 4);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(44100);
    u32(44100 * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(frames * 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (const float v : {l[i], r[i]})
            u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

Stereo stretch(const Stereo& in, const char* backend, int tier, double timeRatio, double pitch)
{
    auto s = engine::voice::StretcherRegistry::builtin().find(backend)();
    s->prepare(kRate, 64, 2);
    if (auto* g = dynamic_cast<engine::voice::stretch::GrainStretcher*>(s.get()))
        g->setQuality(tier);
    s->setRatios(timeRatio, pitch);
    s->reset();
    // Two passes of the loop, as a looping track would play it.
    const auto outFrames = static_cast<std::size_t>(2 * kFrames * timeRatio);
    Stereo o {std::vector<float>(outFrames), std::vector<float>(outFrames)};
    std::vector<float> a(1 << 15), b(1 << 15);
    std::size_t pos = 0;
    for (std::size_t at = 0; at < outFrames; at += 64)
    {
        const int n = static_cast<int>(std::min<std::size_t>(64, outFrames - at));
        const int need = std::min<int>(s->framesNeeded(n), static_cast<int>(a.size()));
        for (int i = 0; i < need; ++i, ++pos)
        {
            const std::size_t k = pos % static_cast<std::size_t>(kFrames);
            a[static_cast<std::size_t>(i)] = pos < 2u * kFrames ? in.l[k] : 0.0f;
            b[static_cast<std::size_t>(i)] = pos < 2u * kFrames ? in.r[k] : 0.0f;
        }
        const float* src[2] = {a.data(), b.data()};
        float* dst[2] = {o.l.data() + at, o.r.data() + at};
        s->process(src, need, dst, n);
    }
    return o;
}
} // namespace

TEST_CASE("bench_stretch_render: listening set for the backend decision", "[.listening]")
{
    const std::filesystem::path dir = std::filesystem::path(APP_ARTIFACT_DIR).parent_path() / "stretch-listening";
    std::filesystem::create_directories(dir);
    struct Stimulus
    {
        const char* name;
        Stereo s;
    };
    const Stimulus stimuli[] = {{"drums", normalised(drums())}, {"vocal", normalised(vocal())},
                                  {"pad", normalised(pad())}, {"bass", normalised(bass())}};
    struct Case
    {
        const char* name;
        double projectBpm;
        int semitones;
    };
    const Case cases[] = {{"tempo-25", 90.0, 0},  {"tempo-10", 108.0, 0}, {"tempo+10", 132.0, 0},
                          {"tempo+25", 150.0, 0}, {"pitch-7", 120.0, -7}, {"pitch+7", 120.0, 7}};
    struct Backend
    {
        const char* name;
        int tier;
    };
    const Backend backends[] = {{"pvoc", 0}, {"pvoc", 1}, {"wsola", 0}, {"wsola", 1}, {"none", 0}};
    std::ofstream key(dir / "key.csv");
    key << "file,backend,tier,projectBpm,sampleBpm,semitones\n";
    std::mt19937 rng(0x5EED91u);
    int files = 0;
    for (const auto& st : stimuli)
    {
        writeWav(dir / (std::string(st.name) + "_original.wav"), st.s.l, st.s.r);
        for (const auto& c : cases)
        {
            std::vector<int> order {0, 1, 2, 3, 4};
            std::shuffle(order.begin(), order.end(), rng);
            for (int slot = 0; slot < 5; ++slot)
            {
                const Backend& b = backends[order[static_cast<std::size_t>(slot)]];
                if (std::string(b.name) == "none" && c.semitones != 0)
                    continue;
                const double t = 120.0 / c.projectBpm;
                const Stereo out = stretch(st.s, b.name, b.tier, t, std::exp2(c.semitones / 12.0));
                const std::string file = std::string(st.name) + "_" + c.name + "_" + static_cast<char>('A' + slot) + ".wav";
                writeWav(dir / file, out.l, out.r);
                key << file << "," << b.name << "," << b.tier << "," << c.projectBpm << ",120," << c.semitones << "\n";
                ++files;
            }
        }
    }
    CHECK(files == 4 * (4 * 5 + 2 * 4));
}
