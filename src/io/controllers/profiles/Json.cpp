#include "io/controllers/profiles/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace io::controllers::json
{
namespace
{
constexpr int kMaxDepth = 32;

struct Reader
{
    std::string_view s;
    std::size_t i = 0;
    std::string error;

    bool fail(const char* what)
    {
        if (error.empty())
        {
            int line = 1;
            for (std::size_t k = 0; k < i && k < s.size(); ++k)
                line += s[k] == '\n' ? 1 : 0;
            error = std::string(what) + " at line " + std::to_string(line);
        }
        return false;
    }
    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    bool literal(std::string_view word)
    {
        if (s.substr(i, word.size()) != word)
            return fail("unexpected token");
        i += word.size();
        return true;
    }
    static void utf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& v)
    {
        if (i + 4 > s.size())
            return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& out)
    {
        ++i; // opening quote
        while (i < s.size())
        {
            const char c = s[i++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i >= s.size())
                break;
            const char e = s[i++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                std::uint32_t cp = 0;
                if (! hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
                {
                    i += 2;
                    std::uint32_t lo = 0;
                    if (! hex4(lo))
                        return false;
                    if (lo >= 0xDC00 && lo < 0xE000)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        return fail("bad surrogate pair");
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool num(Value& v)
    {
        const std::size_t start = i;
        if (i < s.size() && s[i] == '-')
            ++i;
        bool digits = false;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+'
                                || s[i] == '-'))
        {
            digits = true;
            ++i;
        }
        if (! digits)
            return fail("bad number");
        const std::string text(s.substr(start, i - start));
        char* end = nullptr;
        v.number = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != 0 || ! std::isfinite(v.number))
            return fail("bad number");
        v.type = Value::Type::Number;
        return true;
    }
    bool value(Value& v, int depth)
    {
        if (depth > kMaxDepth)
            return fail("nested too deeply");
        ws();
        if (i >= s.size())
            return fail("unexpected end");
        const char c = s[i];
        if (c == '{')
        {
            ++i;
            v.type = Value::Type::Object;
            ws();
            if (i < s.size() && s[i] == '}')
            {
                ++i;
                return true;
            }
            for (;;)
            {
                ws();
                if (i >= s.size() || s[i] != '"')
                    return fail("expected a member name");
                std::string key;
                if (! str(key))
                    return false;
                ws();
                if (i >= s.size() || s[i] != ':')
                    return fail("expected ':'");
                ++i;
                Value member;
                if (! value(member, depth + 1))
                    return false;
                bool seen = false;
                for (const auto& k : v.keys)
                    seen = seen || k == key;
                if (! seen)
                {
                    v.keys.push_back(std::move(key));
                    v.items.push_back(std::move(member));
                }
                ws();
                if (i < s.size() && s[i] == ',')
                {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}')
                {
                    ++i;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[')
        {
            ++i;
            v.type = Value::Type::Array;
            ws();
            if (i < s.size() && s[i] == ']')
            {
                ++i;
                return true;
            }
            for (;;)
            {
                Value item;
                if (! value(item, depth + 1))
                    return false;
                v.items.push_back(std::move(item));
                ws();
                if (i < s.size() && s[i] == ',')
                {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']')
                {
                    ++i;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"')
        {
            v.type = Value::Type::String;
            return str(v.string);
        }
        if (c == 't')
        {
            v.type = Value::Type::Bool;
            v.boolean = true;
            return literal("true");
        }
        if (c == 'f')
        {
            v.type = Value::Type::Bool;
            return literal("false");
        }
        if (c == 'n')
            return literal("null");
        return num(v);
    }
};
} // namespace

const Value* Value::get(std::string_view key) const noexcept
{
    if (type != Type::Object)
        return nullptr;
    for (std::size_t k = 0; k < keys.size(); ++k)
        if (keys[k] == key)
            return &items[k];
    return nullptr;
}

core::Result<Value> parse(std::string_view text)
{
    Reader r {text, 0, {}};
    Value v;
    if (! r.value(v, 0))
        return core::Error {"The file is not valid JSON.", r.error};
    r.ws();
    if (r.i != text.size())
    {
        r.fail("text after the value");
        return core::Error {"The file is not valid JSON.", r.error};
    }
    return v;
}

std::string quote(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            }
            else
                out += c;
        }
    }
    return out + "\"";
}

std::string number(double v)
{
    char buf[32];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15)
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    else
        std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}
} // namespace io::controllers::json
