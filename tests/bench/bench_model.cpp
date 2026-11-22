// Model benchmark (default construction ≤ 5 ms on the host; report sizeof(Project)). Also
// times copy (the undo/save snapshot cost, budget ≤ 5 ms), validate(), path parse/format and the enum
// display lookup (paint code). Median of N runs, std::chrono::steady_clock; Release only (host-release preset).
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/model/paths/ParamPath.h"
#include "core/params/ParamRegistry.h"
#include "tests/bench/harness/Bench.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace core::model;

namespace
{
template <typename Fn>
double medianMs(int runs, Fn&& fn)
{
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(runs));
    for (int i = 0; i < runs; ++i)
    {
        const auto a = std::chrono::steady_clock::now();
        fn();
        const auto b = std::chrono::steady_clock::now();
        t.push_back(std::chrono::duration<double, std::milli>(b - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

volatile std::size_t sink = 0;
} // namespace

TEST_CASE("model default construction, copy, validate", "[bench][model]")
{
    std::printf("BENCH model sizeof(Project)=%zu sizeof(Scene)=%zu sizeof(Pattern)=%zu sizeof(Track)=%zu "
                "sizeof(Step)=%zu\n",
                sizeof(Project), sizeof(Scene), sizeof(Pattern), sizeof(Track), sizeof(Step));

    const double construct = medianMs(31, [] {
        auto p = newDefaultProject();
        sink = sink + static_cast<std::size_t>(p->scenes[15].tracks[15].colour);
    });
    auto base = newDefaultProject();
    const double copy = medianMs(31, [&] {
        auto c = std::make_unique<Project>(*base);
        sink = sink + c->scenes[0].patterns[0].steps.size();
    });
    const double reset = medianMs(31, [&] { resetToDefaults(*base); });
    std::vector<std::string> problems;
    bool ok = true;
    const double check = medianMs(31, [&] {
        problems.clear();
        ok = validate(*base, problems) && ok;
    });
    std::printf("BENCH model newDefaultProject=%.3f ms copy=%.3f ms resetToDefaults=%.3f ms validate=%.3f ms\n",
                construct, copy, reset, check);
    bench::record("model_new_default_project", bench::msToCycles(5.0), bench::msToCycles(construct));
    bench::record("model_copy", bench::msToCycles(5.0), bench::msToCycles(copy));
    bench::record("model_reset_to_defaults", bench::msToCycles(5.0), bench::msToCycles(reset));
    bench::record("model_validate", bench::msToCycles(5.0), bench::msToCycles(check)); // our budget = the copy's
    CHECK(ok);
    CHECK(construct <= 5.0); // construction budget
    CHECK(copy <= 5.0);      // snapshot budget
}

TEST_CASE("model paths and enum lookups", "[bench][model]")
{
    const auto& r = core::builtinParams();
    REQUIRE(r.size() > 0);
    std::vector<std::string> texts;
    for (std::size_t i = 0; i < r.size(); ++i)
    {
        const ParamPath p {r.all()[i].scope == core::ParamScope::Track ? Scope::Track : Scope::Scene, 3, 7, 0, 0, 0,
                           core::ParamId {static_cast<std::uint16_t>(i)}};
        auto t = formatPath(p, r);
        if (! t.empty())
            texts.push_back(std::move(t));
    }
    REQUIRE_FALSE(texts.empty());
    constexpr int kReps = 2000;
    const double parse = medianMs(9, [&] {
        for (int k = 0; k < kReps; ++k)
            for (const auto& t : texts)
                sink = sink + parsePath(t, r)->param.value;
    });
    const auto first = *parsePath(texts.front(), r);
    const double format = medianMs(9, [&] {
        for (int k = 0; k < kReps; ++k)
            sink = sink + formatPath(first, r).size();
    });
    const double display = medianMs(9, [&] {
        for (int k = 0; k < kReps * 100; ++k)
            sink = sink + displayName(static_cast<EffectType>(k % 13)).size();
    });
    const double perParse = parse * 1e6 / (kReps * static_cast<double>(texts.size()));
    std::printf("BENCH model parsePath=%.0f ns (%zu specs) formatPath=%.0f ns displayName=%.1f ns\n", perParse,
                r.size(), format * 1e6 / kReps, display * 1e6 / (kReps * 100));
    bench::record("model_parse_path", 100000.0, perParse);
    bench::record("model_format_path", 100000.0, format * 1e6 / kReps);
    CHECK(perParse < 100000.0); // sanity: message-thread operation, not a budgeted path
}
