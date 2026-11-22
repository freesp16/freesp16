// FX-1305: the app's own oscillator random table, generated from its formula at compile time (see the header).
#include "engine/fx/effects/OscillatorRandomTable.h"

namespace engine::fx
{
namespace
{
constexpr std::array<float, kOscRandomTableSize> build() noexcept
{
    std::array<float, kOscRandomTableSize> t{};
    std::uint32_t x = kOscRandomSeed;
    for (auto& v : t)
    {
        x = oscRandomNext(x);
        v = oscRandomValue(x);
    }
    return t;
}
} // namespace

constexpr std::array<float, kOscRandomTableSize> kTable = build();
const std::array<float, kOscRandomTableSize> kOscRandomTable = kTable;
static_assert(kTable[0] >= -1.0f && kTable[0] < 1.0f, "FX-1305 range");
} // namespace engine::fx
