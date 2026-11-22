#include "services/store/NativeCodecJson.h"

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace services::store::detail
{
void appendEscaped(std::string& out, std::string_view s)
{
    out.push_back('"');
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                }
                else
                    out.push_back(ch);
        }
    }
    out.push_back('"');
}

void JsonWriter::newline(int depth)
{
    out_.push_back('\n');
    out_.append(static_cast<std::size_t>(2 * depth), ' ');
}

void JsonWriter::beforeValue()
{
    if (afterKey_)
    {
        afterKey_ = false;
        return;
    }
    if (depth_ == 0)
        return;
    auto& f = stack_[static_cast<std::size_t>(depth_)];
    if (! f.first)
        out_.push_back(',');
    if (f.inl && ! f.first && ! compact_)
        out_.push_back(' ');
    else if (! f.inl && ! compact_)
        newline(depth_);
    f.first = false;
}

void JsonWriter::beginObject(Layout layout)
{
    beforeValue();
    const bool parentInline = depth_ > 0 && stack_[static_cast<std::size_t>(depth_)].inl;
    stack_[static_cast<std::size_t>(++depth_)] = {layout == Layout::Inline || parentInline, true};
    out_.push_back('{');
}

void JsonWriter::endObject()
{
    const auto f = stack_[static_cast<std::size_t>(depth_--)];
    if (! f.first && ! compact_)
    {
        if (f.inl)
            out_.push_back(' ');
        else
            newline(depth_);
    }
    out_.push_back('}');
}

void JsonWriter::beginArray(Layout layout)
{
    beforeValue();
    const bool parentInline = depth_ > 0 && stack_[static_cast<std::size_t>(depth_)].inl;
    stack_[static_cast<std::size_t>(++depth_)] = {layout == Layout::Inline || parentInline, true};
    out_.push_back('[');
}

void JsonWriter::endArray()
{
    const auto f = stack_[static_cast<std::size_t>(depth_--)];
    if (! f.first && ! f.inl && ! compact_)
        newline(depth_);
    out_.push_back(']');
}

void JsonWriter::key(std::string_view k)
{
    auto& f = stack_[static_cast<std::size_t>(depth_)];
    if (! f.first)
        out_.push_back(',');
    if (! compact_)
    {
        if (f.inl)
            out_.push_back(' ');
        else
            newline(depth_);
    }
    f.first = false;
    appendEscaped(out_, k);
    out_ += compact_ ? ":" : ": ";
    afterKey_ = true;
}

void JsonWriter::value(bool v)
{
    beforeValue();
    out_ += v ? "true" : "false";
}

void JsonWriter::value(std::int64_t v)
{
    beforeValue();
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out_.append(buf, r.ptr);
}

void JsonWriter::value(double v)
{
    beforeValue();
    if (! std::isfinite(v))
    {
        out_ += "null"; // never produced by a valid model (validate() checks finiteness)
        return;
    }
    char buf[40];
    auto r = std::to_chars(buf, buf + sizeof buf, v); // shortest form that reads back exactly
    // juce::JSON keeps 18 digits and counts the zeros after "0." among them (CharacterFunctions::readDoubleValue):
    // 0.0014224867281911503 would lose its last digits, 1.4224867281911503e-03 does not.
    int digits = 0;
    bool leading = true;
    for (const char* c = buf; c < r.ptr && *c != 'e'; ++c)
        if (*c >= '0' && *c <= '9' && ! (leading && digits == 0 && *c == '0'))
            ++digits;
        else if (*c == '.')
            leading = false;
    if (digits > 17)
        r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    out_.append(buf, r.ptr);
}

void JsonWriter::value(float v)
{
    if (! std::isfinite(v))
        return value(static_cast<double>(v));
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof buf - 1, v);
    *r.ptr = 0;
    // The reader parses a double and rounds it to float: keep the short form only when that gives v back.
    if (static_cast<float>(std::strtod(buf, nullptr)) != v)
        return value(static_cast<double>(v));
    beforeValue();
    out_.append(buf, r.ptr);
}

void JsonWriter::value(std::string_view s)
{
    beforeValue();
    appendEscaped(out_, s);
}

void JsonWriter::null()
{
    beforeValue();
    out_ += "null";
}

void JsonWriter::raw(std::string_view json)
{
    beforeValue();
    out_ += json;
}

void JsonWriter::var(const juce::var& v)
{
    if (v.isVoid() || v.isUndefined())
        null();
    else if (v.isBool())
        value(static_cast<bool>(v));
    else if (v.isInt() || v.isInt64())
        value(static_cast<std::int64_t>(static_cast<juce::int64>(v)));
    else if (v.isDouble())
        value(static_cast<double>(v));
    else if (v.isString())
        value(std::string_view(v.toString().toStdString()));
    else if (const auto* array = v.getArray())
    {
        beginArray(Layout::Inline);
        for (const auto& item : *array)
            var(item);
        endArray();
    }
    else if (const auto* object = v.getDynamicObject())
    {
        beginObject(Layout::Inline);
        for (const auto& property : object->getProperties())
        {
            key(property.name.toString().toStdString());
            var(property.value);
        }
        endObject();
    }
    else
        null(); // methods, binary data: not JSON
}

std::string canonicalJson(const juce::var& v)
{
    std::string out;
    JsonWriter w(out, true);
    w.var(v);
    return out;
}
} // namespace services::store::detail
