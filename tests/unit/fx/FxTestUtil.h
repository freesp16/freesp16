// Shared helpers of the FX unit tests: the built-in registry with the test effects enabled, a bank built from the
// built-in ParamSpecs, blocks of test signal, and the process heap in use (for the memory budget).
#pragma once

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxSlotBank.h"

#include <cstddef>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace fxtest
{
inline constexpr std::string_view kTestFlags[] = {engine::fx::kTestEffectFlag};

inline std::vector<std::string_view> builtinIds()
{
    std::vector<std::string_view> ids;
    for (const auto& s : core::builtinParams().all())
        ids.push_back(s.id);
    return ids;
}

inline void buildBank(engine::fx::FxSlotBank& bank, double fs = 44100.0)
{
    const auto ids = builtinIds();
    const auto k = core::fx::internalIndexTable(core::builtinParams());
    bank.build(engine::fx::EffectRegistry::builtin(), ids, kTestFlags, k);
    bank.prepare(fs, engine::kBlockFrames);
}

inline core::ParamId idOf(std::string_view id)
{
    return *core::builtinParams().idOf(id);
}

inline engine::ParamChange change(std::string_view id, float v, std::uint16_t scope = 0)
{
    return engine::ParamChange{idOf(id), scope, v, 0};
}

inline engine::fx::ProcessContext context(double fs = 44100.0, double bpm = 120.0)
{
    engine::fx::ProcessContext c{fs, bpm, false, 0u, {}, 0};
    c.triggerOffset.fill(-1);
    return c;
}

// Bytes the process heap has handed out (live allocations).
inline std::size_t heapInUse()
{
#if defined(__APPLE__)
    malloc_statistics_t s{};
    malloc_zone_statistics(nullptr, &s);
    return s.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    return mallinfo2().uordblks;
#else
    return 0;
#endif
}
} // namespace fxtest
