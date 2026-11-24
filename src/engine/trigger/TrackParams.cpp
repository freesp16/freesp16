#include "engine/trigger/TrackParams.h"

namespace engine::trigger
{
namespace
{
int roundIndex(float v, int lo, int hi) noexcept
{
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    return i < lo ? lo : (i > hi ? hi : i);
}

template <typename T, typename V> bool store(T& field, V value) noexcept
{
    const T v = static_cast<T>(value);
    if (field == v)
        return false;
    field = v;
    return true;
}
} // namespace

bool applyTrackParam(TrackSettings& s, TrackField field, float value) noexcept
{
    bool scale = false;
    switch (field)
    {
        case TrackField::Mode: return store(s.mode, roundIndex(value, 0, 4));
        case TrackField::TriggerMode: return store(s.triggerMode, roundIndex(value, 0, 1));
        case TrackField::ChokeGroup: return store(s.chokeGroup, roundIndex(value, 0, 8));
        case TrackField::InputSource: return store(s.input, roundIndex(value, 0, 3));
        case TrackField::ScaleType: scale = store(s.scaleType, roundIndex(value, 0, core::scales::kScales - 1)); break;
        case TrackField::KeyShift:
            scale = store(s.keyShift, roundIndex(value, core::scales::kMinKeyShift, core::scales::kMaxKeyShift));
            break;
        case TrackField::ScaleArea:
            scale = store(s.window, roundIndex(value, core::scales::kMinWindow, core::scales::kMaxWindow));
            break;
        case TrackField::MidiScaleType:
            if (! store(s.midiScaleType, roundIndex(value, 0, core::scales::kScales - 1)))
                return false;
            s.midiPadSemitones = core::scales::padTable(core::scales::scaleMask(s.midiScaleType), 0, s.midiWindow);
            return true;
        case TrackField::MidiScaleRoot: return store(s.midiRoot, roundIndex(value, 0, 119)); // C0…B9
        case TrackField::MidiScaleArea:
            if (! store(s.midiWindow, roundIndex(value, core::scales::kMinWindow, core::scales::kMaxWindow)))
                return false;
            s.midiPadSemitones = core::scales::padTable(core::scales::scaleMask(s.midiScaleType), 0, s.midiWindow);
            return true;
        case TrackField::Count: return false;
    }
    if (scale)
        s.padSemitones = core::scales::padTable(core::scales::scaleMask(s.scaleType), s.keyShift, s.window);
    return scale;
}

bool applyPadParam(PadState& p, PadField field, float value) noexcept
{
    switch (field)
    {
        case PadField::Mode: return store(p.mode, roundIndex(value, 0, 4));
        case PadField::Selected: return store(p.selected, roundIndex(value, 0, 15));
        case PadField::Shift: return store(p.shift, value >= 0.5f);
        case PadField::Count: break;
    }
    return false;
}
} // namespace engine::trigger
