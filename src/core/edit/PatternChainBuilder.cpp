#include "core/edit/PatternChainBuilder.h"

namespace core::edit
{
void PatternChainBuilder::begin(int scene, int currentPattern) noexcept
{
    plan_ = ChainPlan {};
    plan_.scene = static_cast<std::uint8_t>(scene & 15);
    plan_.patterns[0] = static_cast<std::uint8_t>(currentPattern & 15);
    plan_.length = 1;
    holding_ = true;
}

bool PatternChainBuilder::add(int pattern) noexcept
{
    if (! holding_ || plan_.length >= core::chain::kMaxChain)
        return false;
    plan_.patterns[plan_.length] = static_cast<std::uint8_t>(pattern & 15);
    ++plan_.length;
    return true;
}

std::optional<ChainPlan> PatternChainBuilder::end() noexcept
{
    if (! holding_)
        return std::nullopt;
    holding_ = false;
    if (plan_.length < 2)
        return std::nullopt; // only the held key: a plain press
    return plan_;
}
} // namespace core::edit
