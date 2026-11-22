#include "platform/common/display/GlBinding.h"

namespace platform::common
{
namespace
{
GlBinding& binding() noexcept
{
    static GlBinding b;
    return b;
}
} // namespace

void setGlBinding(GlBinding b) noexcept { binding() = b; }
const GlBinding& glBinding() noexcept { return binding(); }
} // namespace platform::common
