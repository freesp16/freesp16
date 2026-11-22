#include "core/util/sequencer/PatternImage.h"

namespace core::sequencer
{
namespace
{
PatternCompiler& compiler() noexcept
{
    static PatternCompiler c; // function-local: registration order across TUs cannot reach it uninitialised
    return c;
}
} // namespace

void setPatternCompiler(PatternCompiler c) noexcept { compiler() = c; }
PatternCompiler patternCompiler() noexcept { return compiler(); }
} // namespace core::sequencer
