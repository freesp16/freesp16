#include "io/controllers/profiles/Mapper.h"

#include "io/controllers/ControlNames.h"

#include <algorithm>
#include <cmath>

namespace io::controllers
{
namespace
{
constexpr int kNoteClass = 0, kCcClass = 1, kOtherClass = 2; // other: number 0 = pitch bend, 1 = channel pressure
constexpr int kPitchBendNumber = 0, kPressureNumber = 1;

int classOf(std::uint8_t st) noexcept
{
    return st == status::note ? kNoteClass : st == status::cc ? kCcClass : kOtherClass;
}

int relative(Encoding e, int v) noexcept
{
    switch (e)
    {
    case Encoding::TwosComplement: return v < 64 ? v : v - 128;
    case Encoding::BinaryOffset: return v - 64;
    case Encoding::SignMagnitude: return (v & 64) != 0 ? -(v & 63) : (v & 63);
    default: return 0;
    }
}
} // namespace

std::array<std::uint8_t, 128> velocityTable(const PadResponse& r) noexcept
{
    std::array<std::uint8_t, 128> t {};
    const int threshold = std::clamp(r.threshold, 1, 16), sensitivity = std::clamp(r.sensitivity, 1, 16);
    const int shape = static_cast<int>(r.shape);
    const double exponent = shape < 0 ? 1.0 + (-shape) / 2.0 : shape > 0 && shape <= 3 ? 1.0 / (1.0 + shape / 2.0) : 1.0;
    for (int v = 1; v < 128; ++v)
    {
        if (v < (threshold - 1) * 4)
            continue; // below PAD THRESHOLD: no hit
        if (r.shape == PadCurveShape::Fixed)
        {
            t[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(std::clamp(r.fixedVelocity, 1, 127));
            continue;
        }
        const double x = std::min(1.0, v / 127.0 * sensitivity / 8.0);
        const int out = static_cast<int>(std::lround(std::pow(x, exponent) * 127.0));
        t[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(std::clamp(out, 1, 127));
    }
    return t;
}

ControllerMapper::ControllerMapper(const LoadedProfile& p, std::uint8_t source)
    : bindings_(p.in), state_(p.in.size()), source_(source), padBank_(p.padBank)
{
    // Channel-specific bindings win over "any channel" ones: fill the "any" ones first.
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < bindings_.size(); ++i)
        {
            const auto& b = bindings_[i];
            if ((b.channel == 0) != (pass == 0))
                continue;
            const auto index = static_cast<std::uint16_t>(i + 1);
            if (b.status == status::realtime)
            {
                if (b.number >= 0xFA && b.number <= 0xFC)
                    realtime_[static_cast<std::size_t>(b.number - 0xFA)] = index;
                continue;
            }
            if (b.status == status::mmc)
            {
                mmc_[b.number & 127u] = index;
                continue;
            }
            const int cls = classOf(b.status);
            const int number = b.status == status::pitchBend ? kPitchBendNumber
                             : b.status == status::pressure ? kPressureNumber
                                                            : b.number;
            if (b.channel == 0)
                for (int ch = 0; ch < kChannels; ++ch)
                    put(cls, ch, number, index);
            else
                put(cls, b.channel - 1, number, index);
        }
    setResponse({});
}

void ControllerMapper::put(int cls, int channel, int number, std::uint16_t index) noexcept
{
    table_[slot(cls, channel, number & 127)] = index;
}

void ControllerMapper::setResponse(const PadResponse& r) noexcept
{
    const auto t = velocityTable(r);
    for (std::size_t i = 0; i < t.size(); ++i)
        velocity_[i].store(t[i], std::memory_order_relaxed);
}

void ControllerMapper::setTrim(int pad0, int trim) noexcept
{
    if (pad0 >= 0 && pad0 < 16)
        trim_[static_cast<std::size_t>(pad0)].store(static_cast<std::int8_t>(std::clamp(trim, -20, 20)),
                                                    std::memory_order_relaxed);
}

const MidiBinding* ControllerMapper::bindingFor(std::uint8_t st, std::uint8_t number) const noexcept
{
    const int hi = st & 0xF0, ch = st & 0x0F;
    std::uint16_t index = 0;
    if (st >= 0xFA && st <= 0xFC)
        index = realtime_[static_cast<std::size_t>(st - 0xFA)];
    else if (hi == 0x80 || hi == 0x90 || hi == 0xA0)
        index = table_[slot(kNoteClass, ch, number & 127)];
    else if (hi == 0xB0)
        index = table_[slot(kCcClass, ch, number & 127)];
    else if (hi == 0xE0)
        index = table_[slot(kOtherClass, ch, kPitchBendNumber)];
    else if (hi == 0xD0)
        index = table_[slot(kOtherClass, ch, kPressureNumber)];
    return index == 0 ? nullptr : &bindings_[index - 1u];
}

void ControllerMapper::pad(const MidiBinding& b, int velocity, double t, MapResult& out,
                           engine::TriggerSink* triggers) noexcept
{
    const int pad0 = indexOf(b.control) - 1;
    auto& as = padAs_[static_cast<std::size_t>(pad0)];
    if (velocity <= 0)
    {
        if (as == 2)
            out.add(event(ControlEvent::Kind::Release, step(pad0 + 1), 0, t));
        else if (as == 1)
            out.add(event(ControlEvent::Kind::Release, b.control, 0, t));
        as = 0;
        return;
    }
    if (as != 0)
        return; // a second note on without a note off: the pad is already down
    if (padBank_ && bankHeld_)
    {
        as = 2;
        out.add(event(ControlEvent::Kind::Press, step(pad0 + 1), 0, t));
        return;
    }
    int v = velocity_[static_cast<std::size_t>(velocity & 127)].load(std::memory_order_relaxed);
    if (v == 0)
        return; // below the threshold
    v = std::clamp(v + trim_[static_cast<std::size_t>(pad0)].load(std::memory_order_relaxed), 1, 127);
    as = 1;
    lastVelocity_.store(v, std::memory_order_relaxed);
    if (triggers != nullptr) // UI-118: straight into the engine from this thread
    {
        triggers->push({static_cast<std::uint8_t>(pad0), static_cast<std::uint8_t>(v), 60, static_cast<std::int8_t>(pad0)});
        out.triggered = true;
    }
    out.add(event(ControlEvent::Kind::Press, b.control, v, t));
}

void ControllerMapper::strip(int position, int contact, double t, MapResult& out) noexcept
{
    if (position >= 0)
        position_ = position;
    bool touching = false;
    if (contact >= 0) // the contact CC alone decides when it changes
    {
        touchCc_ = contact != 0;
        touching = touchCc_;
    }
    else // pitch bend: off centre = contact, or the contact CC held
        touching = touchCc_ || position_ != 64;
    using Kind = ControlEvent::Kind;
    if (touching && ! contact_)
    {
        contact_ = true;
        out.add(event(Kind::Touch, Control::Strip, position_, t));
    }
    else if (touching && position >= 0)
        out.add(event(Kind::Value, Control::Strip, position_, t));
    else if (! touching && contact_)
    {
        contact_ = false;
        out.add(event(Kind::Release, Control::Strip, position_, t));
    }
}

// One bound value: `value` in `bits` (7 or 14) for absolute sources, `on` for note-style sources.
void ControllerMapper::control(const MidiBinding& b, State& s, int value, int bits, bool on, double t, MapResult& out,
                               engine::TriggerSink* triggers) noexcept
{
    using Kind = ControlEvent::Kind;
    if (b.function == Function::PadBank)
    {
        bankHeld_ = b.enc == Encoding::Note ? on : value >= 64;
        return;
    }
    const auto info = controlInfo(b.control);
    switch (info.kind)
    {
    case ControlKind::Pad:
        pad(b, b.enc == Encoding::Note ? (on ? value : 0) : value, t, out, triggers);
        return;
    case ControlKind::Button:
        if (b.enc == Encoding::Mmc || b.enc == Encoding::Realtime)
        {
            out.add(event(Kind::Press, b.control, 0, t));
            out.add(event(Kind::Release, b.control, 0, t));
            return;
        }
        if (b.enc == Encoding::Absolute)
            on = value >= 64;
        if (on != s.down)
        {
            s.down = on;
            out.add(event(on ? Kind::Press : Kind::Release, b.control, 0, t));
        }
        return;
    case ControlKind::Encoder:
    {
        int detents = relative(b.enc, value);
        if (b.enc == Encoding::Absolute || b.enc == Encoding::PitchBend14)
        {
            const int v7 = bits == 14 ? value >> 7 : value;
            const int last = s.last;
            s.last = static_cast<std::int16_t>(v7);
            if (last < 0)
                return; // the first value only sets the reference
            detents = v7 - last;
            if (detents >= 64 || detents <= -64)
                return; // a wrap (127 → 0): ignored (UI-132)
        }
        if (detents != 0)
            out.add(event(Kind::Turn, b.control, std::clamp(detents * b.scale, -32767, 32767), t));
        return;
    }
    case ControlKind::Absolute:
    {
        int v = 0;
        if (b.enc == Encoding::Absolute || b.enc == Encoding::PitchBend14)
            v = bits == 14 ? value * 1023 / 16383 : value * 1023 / 127;
        else
        {
            const int d = relative(b.enc, value) * b.scale;
            if (d == 0)
                return;
            v = std::clamp(s.acc + d, 0, 1023);
            s.acc = static_cast<std::int16_t>(v);
        }
        out.add(event(Kind::Value, b.control, v, t));
        return;
    }
    case ControlKind::Strip:
        if (b.enc == Encoding::Contact)
            strip(-1, value >= 64 ? 1 : 0, t, out);
        else if (b.enc == Encoding::PitchBend14)
            strip(value >> 7, -1, t, out);
        else
        {
            // An absolute CC strip: every value is a touch at that position (no contact CC: it never lifts by itself).
            if (! contact_)
            {
                contact_ = true;
                position_ = value;
                out.add(event(Kind::Touch, Control::Strip, value, t));
            }
            else
            {
                position_ = value;
                out.add(event(Kind::Value, Control::Strip, value, t));
            }
        }
        return;
    default: return;
    }
}

bool ControllerMapper::map(const std::uint8_t* msg, int len, double t, MapResult& out,
                           engine::TriggerSink* triggers) noexcept
{
    out.count = 0;
    out.consumed = false;
    out.triggered = false;
    if (len <= 0)
        return false;
    const std::uint8_t st = msg[0];
    std::uint16_t index = 0;
    int value = 0, bits = 7;
    bool on = false;
    if (st == 0xF0)
    {
        // MMC: F0 7F <device> 06 <command> F7 (any device id)
        if (len < 6 || msg[1] != 0x7F || msg[3] != 0x06)
            return false;
        index = mmc_[msg[4] & 127u];
        on = true;
    }
    else if (st >= 0xFA && st <= 0xFC)
    {
        index = realtime_[static_cast<std::size_t>(st - 0xFA)];
        on = true;
    }
    else if (st >= 0x80 && st < 0xF0)
    {
        const int hi = st & 0xF0, ch = st & 0x0F;
        const int d1 = len > 1 ? msg[1] & 127 : 0, d2 = len > 2 ? msg[2] & 127 : 0;
        switch (hi)
        {
        case 0x80:
        case 0x90:
            index = table_[slot(kNoteClass, ch, d1)];
            on = hi == 0x90 && d2 > 0;
            value = on ? d2 : 0;
            break;
        case 0xA0: // poly aftertouch: PadPressure on an aftertouch pad; consumed (ignored) on any other bound note
            index = table_[slot(kNoteClass, ch, d1)];
            if (index == 0)
                return false;
            if (const auto& b = bindings_[index - 1u]; b.aftertouch && b.function == Function::Control)
            {
                const int pad0 = indexOf(b.control) - 1;
                if (padAs_[static_cast<std::size_t>(pad0)] == 1)
                    out.add(event(ControlEvent::Kind::Pressure, b.control, d2, t));
            }
            out.consumed = true;
            return true;
        case 0xB0:
            index = table_[slot(kCcClass, ch, d1)];
            value = d2;
            break;
        case 0xD0:
            index = table_[slot(kOtherClass, ch, kPressureNumber)];
            value = d1;
            break;
        case 0xE0:
            index = table_[slot(kOtherClass, ch, kPitchBendNumber)];
            value = d1 | d2 << 7;
            bits = 14;
            break;
        default: return false; // program change: never bound
        }
    }
    if (index == 0)
        return false;
    auto& b = bindings_[index - 1u];
    control(b, state_[index - 1u], value, bits, on, t, out, triggers);
    out.consumed = true;
    return true;
}
} // namespace io::controllers
