// How the platform library reaches gldispatch (platform/common/display/gldispatch) without linking it: GlDispatch.cpp
// is compiled into the app executable only (it includes the generated stub list), while the platform library is also
// linked into the host test executables. GlDispatch.cpp registers itself here at static initialisation; without it
// (tests) glBinding().load is null and a GL backend reports "no GL dispatch in this executable".
#pragma once

#include "platform/common/display/gldispatch/GlDispatch.h"

#include <string>

namespace platform::common
{
struct GlBinding
{
    bool (*load)(const char* flavour, const gldispatch::Libraries&, std::string& error) = nullptr;
    std::string (*report)() = nullptr;
};
void setGlBinding(GlBinding) noexcept;
const GlBinding& glBinding() noexcept;
} // namespace platform::common
