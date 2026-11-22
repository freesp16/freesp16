// fx.filter and fx.eq2: engine::fx::effects::MultiModeFilter / ParametricEq in the Reference quirk
// profile against the research models mmf.py / peq.py (tools/golden-gen/goldenlib/models/fx_filter.py, fx_eq2.py; the
// parameters are documented there). "effect" mode runs the effect alone (prepare → profile → [insert defaults] + init →
// reset → per block: events, walk, process); "shell" mode wraps it in engine::fx::FxShell with the registered
// descriptor's ShellPolicy, activated before block 0 (the research EffectBase slot 5 + slot 7).
#include "tests/golden/harness/Subject.h"

#include "engine/fx/EffectRegistry.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/MultiModeFilter.h"
#include "engine/fx/effects/ParametricEq.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace golden
{
namespace
{
using engine::fx::effects::MultiModeFilter;
using engine::fx::effects::ParametricEq;

struct Option
{
    const char* key;
    int which, def, enumSize;
};

struct FilterTraits
{
    using Effect = MultiModeFilter;
    static constexpr const char* id = "filter";
    static constexpr Option options[2] = {{"type", MultiModeFilter::kOptType, 0, 4},
                                          {"slope", MultiModeFilter::kOptSlope, 0, 2}};
    static const float* defaults() { return MultiModeFilter::kDefaults; }
};
struct Eq2Traits
{
    using Effect = ParametricEq;
    static constexpr const char* id = "eq2";
    static constexpr Option options[2] = {{"type1", ParametricEq::kOptType1, ParametricEq::kAppLoShelf, 5},
                                          {"type2", ParametricEq::kOptType2, ParametricEq::kAppHiShelf, 5}};
    static const float* defaults() { return ParametricEq::kDefaults; }
};

struct Change
{
    int knob = -1; // 0…5, or −1 for an option
    int option = -1;
    float value = 0.0f;
    int choice = 0;
    bool reset = false;
};

std::uint32_t xorshift32(std::uint32_t& x) noexcept
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

template <typename T> class FiltSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto& p = c.params;
        if (static_cast<double>(p["fs"]) != c.sampleRate)
            return "params.fs must equal the recipe's sample_rate";
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return std::string("fx.") + T::id + " needs inputs L, R and outputs L, R";
        shellMode_ = p["mode"].toString() == "shell";
        ctx_ = engine::fx::ProcessContext{c.sampleRate, 120.0, false, 0u, {}, 0};
        ctx_.triggerOffset.fill(-1);
        changes_.clear();
        std::vector<Change> init;
        if (p["defaults"].isVoid() || static_cast<bool>(p["defaults"]))
        {
            for (int k = 0; k < 6; ++k)
                init.push_back({k, -1, T::defaults()[k], 0, false});
            for (int i = 0; i < 2; ++i)
                init.push_back({-1, i, 0.0f, T::options[i].def, false});
        }
        if (auto e = parseItem(p["init"], init); ! e.empty())
            return e;
        if (auto* ev = p["events"].getArray())
            for (auto& e : *ev)
            {
                auto& list = changes_[static_cast<int>(e["block"])];
                if (auto err = parseItem(e, list); ! err.empty())
                    return err;
                if (static_cast<bool>(e["reset"]))
                    list.push_back({-1, -1, 0.0f, 0, true});
            }
        if (const auto& w = p["walk"]; w.isObject())
        {
            auto x = static_cast<std::uint32_t>(static_cast<juce::int64>(w["seed"]));
            for (int b = static_cast<int>(w["from"]); b < static_cast<int>(w["to"]); ++b)
                if (auto* keys = w["keys"].getArray())
                    for (auto& kv : *keys)
                    {
                        const auto key = kv.toString();
                        if (xorshift32(x) % 4 != 0)
                            continue;
                        const std::uint32_t r = xorshift32(x);
                        Change ch;
                        if (int o = optionIndex(key); o >= 0)
                        {
                            ch.option = o;
                            ch.choice = static_cast<int>(r % static_cast<std::uint32_t>(T::options[o].enumSize));
                        }
                        else
                        {
                            ch.knob = key.getIntValue();
                            ch.value = static_cast<float>(r >> 8) * (1.0f / 16777216.0f);
                        }
                        walk_[b].push_back(ch);
                    }
        }

        effect_.prepare(c.sampleRate, engine::kBlockFrames);
        if (static_cast<bool>(p["parity_maths"]))
            effect_.setOption(ParametricEq::kOptParityMaths, 1);
        if (shellMode_)
        {
            const auto* d = engine::fx::EffectRegistry::builtin().find(T::id);
            if (d == nullptr)
                return std::string("effect '") + T::id + "' is not registered";
            shell_.prepare(effect_, d->shell, c.sampleRate, engine::fx::SlotKind::Insert, scratchL_, scratchR_);
            shell_.setQuirkProfile(engine::fx::kQuirkProfileReference);
            for (const auto& ch : init)
                apply(ch);
            shell_.activate(); // profile → full parameter set → reset → fade in (FX-024/053/056)
        }
        else
        {
            effect_.setQuirkProfile(engine::fx::kQuirkProfileReference);
            for (const auto& ch : init)
                apply(ch);
            effect_.reset(); // the activation's slot 27
        }
        return {};
    }

    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        if (auto it = changes_.find(block); it != changes_.end())
            for (const auto& ch : it->second)
                apply(ch);
        if (auto it = walk_.find(block); it != walk_.end())
            for (const auto& ch : it->second)
                apply(ch);
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        if (shellMode_)
            shell_.process(out[0], out[1], ctx_);
        else
            effect_.process(out[0], out[1], ctx_);
    }

private:
    static int optionIndex(const juce::String& key)
    {
        for (int i = 0; i < 2; ++i)
            if (key == T::options[i].key)
                return i;
        return -1;
    }
    std::string parseItem(const juce::var& item, std::vector<Change>& out)
    {
        if (! item.isObject())
            return {};
        if (auto* knobs = item["k"].getDynamicObject())
            for (const auto& prop : knobs->getProperties())
            {
                const int k = prop.name.toString().getIntValue();
                if (k < 0 || k > 5)
                    return "knob keys are 0…5";
                out.push_back({k, -1, static_cast<float>(static_cast<double>(prop.value)), 0, false});
            }
        for (int i = 0; i < 2; ++i)
            if (item.hasProperty(T::options[i].key))
                out.push_back({-1, i, 0.0f, static_cast<int>(item[T::options[i].key]), false});
        return {};
    }
    void apply(const Change& ch) noexcept
    {
        if (ch.reset)
            effect_.reset();
        else if (ch.knob >= 0)
        {
            if (shellMode_)
                shell_.setParam(ch.knob, ch.value);
            else
                effect_.setParam(ch.knob, ch.value);
        }
        else if (ch.option >= 0)
        {
            if (shellMode_)
                shell_.setOption(T::options[ch.option].which, ch.choice);
            else
                effect_.setOption(T::options[ch.option].which, ch.choice);
        }
    }

    typename T::Effect effect_;
    engine::fx::FxShell shell_;
    alignas(16) float scratchL_[engine::kBlockFrames]{};
    alignas(16) float scratchR_[engine::kBlockFrames]{};
    bool shellMode_ = false;
    engine::fx::ProcessContext ctx_{};
    std::map<int, std::vector<Change>> changes_, walk_;
};
} // namespace

std::unique_ptr<Subject> makeFilterSubject()
{
    return std::make_unique<FiltSubject<FilterTraits>>();
}

std::unique_ptr<Subject> makeEq2Subject()
{
    return std::make_unique<FiltSubject<Eq2Traits>>();
}
} // namespace golden
