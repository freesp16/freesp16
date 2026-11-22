#include "io/controllers/profiles/Profile.h"

#include "io/controllers/ControlNames.h"
#include "io/controllers/profiles/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <tuple>

namespace io::controllers
{
namespace
{
using json::Value;

constexpr std::string_view kPadBankName = "ext.steps";

struct Named
{
    std::string_view name;
    Encoding enc;
};
constexpr Named kEncodings[] = {{"note", Encoding::Note},         {"absolute", Encoding::Absolute},
                                {"twos", Encoding::TwosComplement}, {"offset", Encoding::BinaryOffset},
                                {"signmag", Encoding::SignMagnitude}, {"pitchbend", Encoding::PitchBend14},
                                {"mmc", Encoding::Mmc},           {"realtime", Encoding::Realtime},
                                {"contact", Encoding::Contact}};

struct MmcName
{
    std::string_view name;
    std::uint8_t command;
};
constexpr MmcName kMmc[] = {{"stop", 0x01}, {"play", 0x02}, {"deferredPlay", 0x03}, {"forward", 0x04},
                            {"rewind", 0x05}, {"record", 0x06}, {"recordExit", 0x07}, {"pause", 0x09}};
struct RealtimeName
{
    std::string_view name;
    std::uint8_t byte;
};
constexpr RealtimeName kRealtime[] = {{"start", 0xFA}, {"continue", 0xFB}, {"stop", 0xFC}};

// The group of a control for "count": first control and size, or {c, 1} for a single control.
std::pair<Control, int> groupOf(Control c) noexcept
{
    if (isPad(c))
        return {Control::Pad1, 16};
    if (isStep(c))
        return {Control::Step1, 16};
    if (isKnob(c))
        return {Control::Knob1, 6};
    if (c >= Control::Bar1 && c <= Control::Bar4)
        return {Control::Bar1, 4};
    return {c, 1};
}
Control offset(Control c, int k) noexcept { return static_cast<Control>(static_cast<int>(c) + k); }

struct Parser
{
    LoadedProfile p;
    std::string error;

    bool fail(const std::string& where, const std::string& what)
    {
        if (error.empty())
            error = where + ": " + what;
        return false;
    }
    static bool integer(const Value* v, int lo, int hi, int& out)
    {
        if (v == nullptr || ! v->isNumber() || v->number != static_cast<double>(static_cast<int>(v->number)))
            return false;
        out = static_cast<int>(v->number);
        return out >= lo && out <= hi;
    }
    bool optInt(const Value& o, std::string_view key, int lo, int hi, int& out, const std::string& where)
    {
        const Value* v = o.get(key);
        if (v == nullptr)
            return true;
        if (! integer(v, lo, hi, out))
            return fail(where, std::string(key) + " must be an integer " + std::to_string(lo) + "…" + std::to_string(hi));
        return true;
    }
    static bool hexColour(std::string_view s, std::uint32_t& out)
    {
        if (! s.empty() && s[0] == '#')
            s.remove_prefix(1);
        if (s.size() != 6)
            return false;
        out = 0;
        for (const char c : s)
        {
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    // The message key of a binding or lamp: status, number, default encoding.
    bool message(const Value& o, const std::string& where, bool lamp, std::uint8_t& st, std::uint8_t& number,
                 Encoding& def)
    {
        int keys = 0, n = 0;
        if (const Value* v = o.get("note"))
        {
            ++keys;
            if (! integer(v, 0, 127, n))
                return fail(where, "note must be 0…127");
            st = status::note;
            number = static_cast<std::uint8_t>(n);
            def = Encoding::Note;
        }
        if (const Value* v = o.get("cc"))
        {
            ++keys;
            if (! integer(v, 0, 127, n))
                return fail(where, "cc must be 0…127");
            st = status::cc;
            number = static_cast<std::uint8_t>(n);
            def = Encoding::Absolute;
        }
        if (! lamp)
        {
            if (const Value* v = o.get("pitchbend"))
            {
                ++keys;
                if (! v->isBool() || ! v->boolean)
                    return fail(where, "pitchbend must be true");
                st = status::pitchBend;
                number = 0;
                def = Encoding::PitchBend14;
            }
            if (const Value* v = o.get("pressure"))
            {
                ++keys;
                if (! v->isBool() || ! v->boolean)
                    return fail(where, "pressure must be true");
                st = status::pressure;
                number = 0;
                def = Encoding::Absolute;
            }
            if (const Value* v = o.get("mmc"))
            {
                ++keys;
                st = status::mmc;
                def = Encoding::Mmc;
                bool found = false;
                if (v->isString())
                    for (const auto& m : kMmc)
                        if (m.name == v->string)
                        {
                            number = m.command;
                            found = true;
                        }
                if (! found && integer(v, 1, 127, n))
                {
                    number = static_cast<std::uint8_t>(n);
                    found = true;
                }
                if (! found)
                    return fail(where, "unknown mmc command");
            }
            if (const Value* v = o.get("realtime"))
            {
                ++keys;
                st = status::realtime;
                def = Encoding::Realtime;
                bool found = false;
                if (v->isString())
                    for (const auto& r : kRealtime)
                        if (r.name == v->string)
                        {
                            number = r.byte;
                            found = true;
                        }
                if (! found)
                    return fail(where, "realtime must be start, stop or continue");
            }
        }
        if (keys != 1)
            return fail(where, lamp ? "needs exactly one of note, cc" : "needs exactly one of note, cc, pitchbend, "
                                                                        "pressure, mmc, realtime");
        return true;
    }

    static bool compatible(const MidiBinding& b, ControlKind kind) noexcept
    {
        const bool relative = b.enc == Encoding::TwosComplement || b.enc == Encoding::BinaryOffset
                           || b.enc == Encoding::SignMagnitude;
        if (b.function == Function::PadBank)
            return b.enc == Encoding::Note || (b.enc == Encoding::Absolute && b.status == status::cc);
        switch (kind)
        {
        case ControlKind::Pad:
            return b.enc == Encoding::Note || (b.enc == Encoding::Absolute && b.status == status::cc);
        case ControlKind::Button:
            return b.enc == Encoding::Note || b.enc == Encoding::Mmc || b.enc == Encoding::Realtime
                || (b.enc == Encoding::Absolute && b.status == status::cc);
        case ControlKind::Encoder:
        case ControlKind::Absolute:
            return relative || b.enc == Encoding::Absolute || b.enc == Encoding::PitchBend14;
        case ControlKind::Strip:
            return b.enc == Encoding::PitchBend14 || b.enc == Encoding::Contact
                || (b.enc == Encoding::Absolute && b.status == status::cc);
        default: return false;
        }
    }

    bool binding(const Value& o, const std::string& where, std::set<std::tuple<int, int, int>>& seen)
    {
        if (! o.isObject())
            return fail(where, "must be an object");
        MidiBinding b;
        Encoding def = Encoding::Note;
        if (! message(o, where, false, b.status, b.number, def))
            return false;
        b.enc = def;
        if (const Value* e = o.get("enc"))
        {
            bool found = false;
            if (e->isString())
                for (const auto& n : kEncodings)
                    if (n.name == e->string)
                    {
                        b.enc = n.enc;
                        found = true;
                    }
            if (! found)
                return fail(where, "unknown enc");
        }
        // The encoding must suit the message.
        const bool okMsg = (b.status == status::note && b.enc == Encoding::Note)
                        || (b.status == status::cc && b.enc != Encoding::Note && b.enc != Encoding::PitchBend14
                            && b.enc != Encoding::Mmc && b.enc != Encoding::Realtime)
                        || (b.status == status::pitchBend && b.enc == Encoding::PitchBend14)
                        || (b.status == status::pressure && b.enc == Encoding::Absolute)
                        || (b.status == status::mmc && b.enc == Encoding::Mmc)
                        || (b.status == status::realtime && b.enc == Encoding::Realtime);
        if (! okMsg)
            return fail(where, "enc " + std::string(encodingName(b.enc)) + " does not suit the message");
        const Value* cv = o.get("control");
        if (cv == nullptr || ! cv->isString())
            return fail(where, "control missing");
        if (cv->string == kPadBankName)
        {
            b.function = Function::PadBank;
            b.control = Control::Count;
        }
        else if (const auto c = controlFromName(cv->string))
            b.control = *c;
        else
            return fail(where, "unknown control '" + cv->string + "'");
        int ch = p.channel, count = 1, scale = 1;
        if (! optInt(o, "channel", 0, 16, ch, where) || ! optInt(o, "count", 1, 16, count, where)
            || ! optInt(o, "scale", -1000, 1000, scale, where))
            return false;
        if (scale == 0)
            return fail(where, "scale must not be 0");
        b.channel = static_cast<std::uint8_t>(b.status >= status::mmc ? 0 : ch);
        b.scale = static_cast<std::int16_t>(scale);
        if (const Value* a = o.get("aftertouch"))
        {
            if (! a->isBool())
                return fail(where, "aftertouch must be true or false");
            b.aftertouch = a->boolean;
            if (b.aftertouch && b.status != status::note)
                return fail(where, "aftertouch needs a note binding");
        }
        if (b.function == Function::Control)
        {
            const auto info = controlInfo(b.control);
            if (! compatible(b, info.kind))
                return fail(where, "enc " + std::string(encodingName(b.enc)) + " cannot drive "
                                       + std::string(nameOf(b.control)));
            if (b.aftertouch && info.kind != ControlKind::Pad)
                return fail(where, "aftertouch needs a pad control");
        }
        else if (! compatible(b, ControlKind::Button))
            return fail(where, "ext.steps needs a note or cc");
        if (count > 1)
        {
            if (b.function != Function::Control)
                return fail(where, "count needs a control group");
            const auto [first, size] = groupOf(b.control);
            const int index = static_cast<int>(b.control) - static_cast<int>(first);
            if (size == 1 || index + count > size)
                return fail(where, "count runs past the control group");
            if (b.status != status::note && b.status != status::cc)
                return fail(where, "count needs note or cc");
            if (static_cast<int>(b.number) + count - 1 > 127)
                return fail(where, "count runs past 127");
        }
        for (int k = 0; k < count; ++k)
        {
            MidiBinding e = b;
            e.number = static_cast<std::uint8_t>(b.number + k);
            if (b.function == Function::Control)
                e.control = offset(b.control, k);
            if (! seen.insert({e.status, e.channel, e.number}).second)
                return fail(where, "a second binding for the same message");
            p.in.push_back(e);
        }
        if (static_cast<int>(p.in.size()) > kMaxBindings)
            return fail(where, "more than " + std::to_string(kMaxBindings) + " bindings");
        return true;
    }

    bool lamp(const Value& o, const std::string& where)
    {
        if (! o.isObject())
            return fail(where, "must be an object");
        LampMap l;
        Encoding def = Encoding::Note;
        if (! message(o, where, true, l.status, l.number, def))
            return false;
        const Value* cv = o.get("control");
        if (cv == nullptr || ! cv->isString())
            return fail(where, "control missing");
        const auto c = controlFromName(cv->string);
        if (! c)
            return fail(where, "unknown control '" + cv->string + "'");
        if (controlInfo(*c).lamp == LampKind::None)
            return fail(where, std::string(nameOf(*c)) + " has no lamp");
        l.control = *c;
        int ch = 0, count = 1;
        if (! optInt(o, "channel", 0, 16, ch, where) || ! optInt(o, "count", 1, 16, count, where))
            return false;
        l.channel = static_cast<std::uint8_t>(ch);
        const Value* pv = o.get("palette");
        if (pv == nullptr || ! pv->isString())
            return fail(where, "palette missing");
        const auto it = std::find(p.paletteNames.begin(), p.paletteNames.end(), pv->string);
        if (it == p.paletteNames.end())
            return fail(where, "unknown palette '" + pv->string + "'");
        l.palette = static_cast<std::uint8_t>(it - p.paletteNames.begin());
        const auto [first, size] = groupOf(l.control);
        const int index = static_cast<int>(l.control) - static_cast<int>(first);
        if (count > 1 && (size == 1 || index + count > size || static_cast<int>(l.number) + count - 1 > 127))
            return fail(where, "count runs past the control group");
        for (int k = 0; k < count; ++k)
        {
            LampMap e = l;
            e.control = offset(l.control, k);
            e.number = static_cast<std::uint8_t>(l.number + k);
            for (const auto& other : p.lamps)
                if (other.control == e.control)
                    return fail(where, "a second lamp for " + std::string(nameOf(e.control)));
            p.lamps.push_back(e);
        }
        return true;
    }

    bool run(std::string_view text)
    {
        auto parsed = json::parse(text);
        if (! parsed)
            return fail("json", parsed.error().detail);
        const Value& root = parsed.value();
        if (! root.isObject())
            return fail("profile", "must be an object");
        int schema = 0;
        if (! integer(root.get("schema"), 1, 1, schema))
            return fail("schema", "must be 1");
        const Value* id = root.get("id");
        if (id == nullptr || ! id->isString() || id->string.empty() || id->string.size() > 64)
            return fail("id", "must be a non-empty string");
        for (const char c : id->string)
            if (! (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
                return fail("id", "only letters, digits, - _ .");
        p.id = id->string;
        const Value* name = root.get("name");
        p.name = name != nullptr && name->isString() && ! name->string.empty() ? name->string : p.id;
        if (const Value* ports = root.get("ports"))
        {
            if (! ports->isString() || ports->string.empty())
                return fail("ports", "must be a non-empty string");
            p.portPattern = ports->string;
        }
        int ch = 0;
        if (! optInt(root, "channel", 0, 16, ch, "channel"))
            return false;
        p.channel = static_cast<std::uint8_t>(ch);
        if (const Value* bank = root.get("padBank"))
        {
            if (! bank->isBool())
                return fail("padBank", "must be true or false");
            p.padBank = bank->boolean;
        }
        if (const Value* pals = root.get("palettes"))
        {
            if (! pals->isObject())
                return fail("palettes", "must be an object");
            for (std::size_t k = 0; k < pals->keys.size(); ++k)
            {
                const auto& arr = pals->items[k];
                const std::string where = "palettes." + pals->keys[k];
                if (! arr.isArray() || arr.items.empty() || arr.items.size() > 128)
                    return fail(where, "must be 1…128 colours");
                std::vector<std::uint32_t> colours;
                for (const auto& c : arr.items)
                {
                    std::uint32_t rgb = 0;
                    std::string_view entry = c.isString() ? std::string_view(c.string) : std::string_view();
                    int value = static_cast<int>(colours.size());
                    if (const auto colon = entry.find(':'); colon != std::string_view::npos)
                    {
                        value = 0;
                        const auto digits = entry.substr(colon + 1);
                        for (const char d : digits)
                            value = d >= '0' && d <= '9' ? value * 10 + (d - '0') : 1000;
                        entry = entry.substr(0, colon);
                        if (digits.empty() || value > 127)
                            return fail(where, "a value after ':' must be 0…127");
                    }
                    if (! c.isString() || ! hexColour(entry, rgb))
                        return fail(where, "colours are \"RRGGBB\" or \"RRGGBB:value\"");
                    colours.push_back(static_cast<std::uint32_t>(value) << 24 | rgb);
                }
                p.paletteNames.push_back(pals->keys[k]);
                p.palettes.push_back(std::move(colours));
            }
            if (p.palettes.size() > 255)
                return fail("palettes", "too many");
        }
        const Value* in = root.get("in");
        if (in == nullptr || ! in->isArray())
            return fail("in", "must be an array");
        std::set<std::tuple<int, int, int>> seen;
        for (std::size_t k = 0; k < in->items.size(); ++k)
            if (! binding(in->items[k], "in[" + std::to_string(k) + "]", seen))
                return false;
        if (const Value* lamps = root.get("lamps"))
        {
            if (! lamps->isArray())
                return fail("lamps", "must be an array");
            for (std::size_t k = 0; k < lamps->items.size(); ++k)
                if (! lamp(lamps->items[k], "lamps[" + std::to_string(k) + "]"))
                    return false;
        }
        bool bankButton = false;
        for (const auto& b : p.in)
            bankButton = bankButton || b.function == Function::PadBank;
        if (bankButton && ! p.padBank)
            return fail("padBank", "an ext.steps binding needs \"padBank\": true");
        return true;
    }
};

char lower(char c) noexcept { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool glob(std::string_view pat, std::string_view s) noexcept
{
    std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size())
    {
        if (p < pat.size() && (pat[p] == '?' || lower(pat[p]) == lower(s[i])))
        {
            ++p;
            ++i;
        }
        else if (p < pat.size() && pat[p] == '*')
        {
            star = p++;
            mark = i;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            i = ++mark;
        }
        else
            return false;
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}

std::string hex(std::uint32_t rgb)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
    return buf;
}
} // namespace

std::string_view encodingName(Encoding e) noexcept
{
    for (const auto& n : kEncodings)
        if (n.enc == e)
            return n.name;
    return "?";
}

Result<LoadedProfile> parseProfile(std::string_view text)
{
    Parser parser;
    if (! parser.run(text))
        return core::Error {"The controller profile is not valid.", parser.error};
    return std::move(parser.p);
}

std::string toJson(const LoadedProfile& p)
{
    std::string s = "{\n  \"schema\": 1,\n  \"id\": " + json::quote(p.id) + ",\n  \"name\": " + json::quote(p.name);
    if (! p.portPattern.empty())
        s += ",\n  \"ports\": " + json::quote(p.portPattern);
    s += ",\n  \"channel\": " + std::to_string(p.channel) + ",\n  \"padBank\": " + (p.padBank ? "true" : "false")
       + ",\n  \"in\": [";
    for (std::size_t k = 0; k < p.in.size(); ++k)
    {
        const auto& b = p.in[k];
        s += k == 0 ? "\n    {" : ",\n    {";
        switch (b.status)
        {
        case status::note: s += "\"note\": " + std::to_string(b.number); break;
        case status::cc: s += "\"cc\": " + std::to_string(b.number); break;
        case status::pitchBend: s += "\"pitchbend\": true"; break;
        case status::pressure: s += "\"pressure\": true"; break;
        case status::mmc: s += "\"mmc\": " + std::to_string(b.number); break;
        default:
            for (const auto& r : kRealtime)
                if (r.byte == b.number)
                    s += "\"realtime\": " + json::quote(r.name);
            break;
        }
        s += ", \"channel\": " + std::to_string(b.channel) + ", \"enc\": " + json::quote(encodingName(b.enc));
        s += ", \"control\": " + json::quote(b.function == Function::PadBank ? kPadBankName : nameOf(b.control));
        if (b.scale != 1)
            s += ", \"scale\": " + std::to_string(b.scale);
        if (b.aftertouch)
            s += ", \"aftertouch\": true";
        s += "}";
    }
    s += p.in.empty() ? "]" : "\n  ]";
    if (! p.palettes.empty())
    {
        s += ",\n  \"palettes\": {";
        for (std::size_t k = 0; k < p.palettes.size(); ++k)
        {
            s += (k == 0 ? "\n    " : ",\n    ") + json::quote(p.paletteNames[k]) + ": [";
            for (std::size_t c = 0; c < p.palettes[k].size(); ++c)
            {
                const auto entry = p.palettes[k][c];
                s += (c == 0 ? "\"" : ", \"") + hex(entry);
                if ((entry >> 24) != c)
                    s += ":" + std::to_string(entry >> 24);
                s += "\"";
            }
            s += "]";
        }
        s += "\n  }";
    }
    if (! p.lamps.empty())
    {
        s += ",\n  \"lamps\": [";
        for (std::size_t k = 0; k < p.lamps.size(); ++k)
        {
            const auto& l = p.lamps[k];
            s += k == 0 ? "\n    {" : ",\n    {";
            s += "\"control\": " + json::quote(nameOf(l.control));
            s += (l.status == status::cc ? ", \"cc\": " : ", \"note\": ") + std::to_string(l.number);
            s += ", \"channel\": " + std::to_string(l.channel) + ", \"palette\": " + json::quote(p.paletteNames[l.palette])
               + "}";
        }
        s += "\n  ]";
    }
    return s + "\n}\n";
}

bool matchesPort(std::string_view pattern, std::string_view portName) noexcept
{
    while (true)
    {
        const auto bar = pattern.find('|');
        if (glob(pattern.substr(0, bar), portName))
            return true;
        if (bar == std::string_view::npos)
            return false;
        pattern.remove_prefix(bar + 1);
    }
}

const LoadedProfile* profileForPort(std::span<const LoadedProfile> profiles, std::string_view portName) noexcept
{
    for (const auto& p : profiles)
        if (! p.portPattern.empty() && ! p.fallback() && matchesPort(p.portPattern, portName))
            return &p;
    for (const auto& p : profiles)
        if (p.fallback())
            return &p;
    return nullptr;
}

const std::vector<LoadedProfile>& builtinProfiles()
{
    static const std::vector<LoadedProfile> parsed = [] {
        std::vector<LoadedProfile> out;
        for (const auto& t : builtinProfileTexts())
            if (auto r = parseProfile(t.json))
            {
                r.value().builtin = true;
                out.push_back(std::move(r.value()));
            }
        return out;
    }();
    return parsed;
}
} // namespace io::controllers
