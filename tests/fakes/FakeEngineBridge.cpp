#include "tests/fakes/FakeEngineBridge.h"

namespace fakes
{
bool FakeEngineBridge::post(const engine::EngineMessage& m)
{
    if (! accept)
        return false;
    posted.push_back(m);
    return true;
}

bool FakeEngineBridge::popEvent(engine::EngineEvent& out)
{
    if (events.empty())
        return false;
    out = events.front();
    events.pop_front();
    return true;
}
} // namespace fakes
