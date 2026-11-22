// Feature flags (UID-024, UID-225): P3 extensions are gated by a flag; with every flag off the UI is exactly the
// reference set of screens. A descriptor with an empty flag is always on. Fixed capacity, no allocation when queried.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace ui
{
class FeatureFlags
{
public:
    static constexpr std::size_t kMax = 32;
    bool enabled(std::string_view flag) const noexcept
    {
        if (flag.empty())
            return true;
        for (std::size_t i = 0; i < count_; ++i)
            if (flags_[i] == flag)
                return true;
        return false;
    }
    // `flag` must have static storage (descriptor literals).
    bool enable(std::string_view flag) noexcept
    {
        if (enabled(flag))
            return true;
        if (count_ == kMax)
            return false;
        flags_[count_++] = flag;
        return true;
    }
    void disable(std::string_view flag) noexcept
    {
        for (std::size_t i = 0; i < count_; ++i)
            if (flags_[i] == flag)
                flags_[i] = flags_[--count_];
    }

private:
    std::array<std::string_view, kMax> flags_{};
    std::size_t count_ = 0;
};
} // namespace ui
