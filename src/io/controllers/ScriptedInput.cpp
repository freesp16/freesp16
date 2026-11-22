// Scripted input (see ScriptedInput.h).
#include "io/controllers/ScriptedInput.h"

#include "io/controllers/AppRequests.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>

namespace io
{
namespace
{
std::string& scriptPath()
{
    static std::string path;
    return path;
}

std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        const std::size_t start = i;
        while (i < line.size() && ! std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i > start)
            out.push_back(line.substr(start, i - start));
    }
    return out;
}

template <typename T> bool number(std::string_view s, T& out) noexcept
{
    if (! s.empty() && s.front() == '+')
        s.remove_prefix(1);
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc {} && end == s.data() + s.size();
}

bool time(std::string_view s, double& out) noexcept
{
    long ms = 0;
    if (number(s, ms) && ms >= 0)
        return out = static_cast<double>(ms), true;
    return false; // integer milliseconds only (std::from_chars for double is not in GCC 11's libstdc++ everywhere)
}

double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::unique_ptr<ControlSource> makeScripted(engine::TriggerSink* triggers)
{
    const std::string& path = scriptPath();
    if (path.empty())
        return nullptr;
    ScriptParseResult script;
    std::ifstream in(path);
    if (! in)
        script.errors.push_back("cannot read " + path);
    else
    {
        std::stringstream ss;
        ss << in.rdbuf();
        script = parseInputScript(ss.str());
    }
    return std::make_unique<ScriptedInputSource>(triggers, std::move(script), path);
}
} // namespace

std::optional<Control> scriptControl(std::string_view name) noexcept
{
    if (auto c = controlFromName(name))
        return c;
    // Short forms: pad5, step3, bar2, knob1 / enc1.
    struct Short
    {
        std::string_view prefix, spec;
    };
    constexpr Short shorts[] = {{"pad", "pad."}, {"step", "step."}, {"bar", "bar."}, {"knob", "enc."}, {"enc", "enc."}};
    for (const auto& s : shorts)
        if (name.size() > s.prefix.size() && name.substr(0, s.prefix.size()) == s.prefix
            && std::isdigit(static_cast<unsigned char>(name[s.prefix.size()])))
        {
            char buf[24];
            const auto digits = name.substr(s.prefix.size());
            if (s.spec.size() + digits.size() >= sizeof buf)
                return std::nullopt;
            std::copy(s.spec.begin(), s.spec.end(), buf);
            std::copy(digits.begin(), digits.end(), buf + s.spec.size());
            return controlFromName(std::string_view(buf, s.spec.size() + digits.size()));
        }
    return std::nullopt;
}

ScriptParseResult parseInputScript(std::string_view text)
{
    ScriptParseResult r;
    int lineNo = 0;
    std::int16_t lastX = 0, lastY = 0;
    while (! text.empty())
    {
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view {} : text.substr(eol + 1);
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        const auto w = words(line);
        if (w.empty())
            continue;
        auto fail = [&](const std::string& why) { r.errors.push_back("line " + std::to_string(lineNo) + ": " + why); };
        ScriptStep step;
        if (! time(w[0], step.atMs))
        {
            fail("bad time '" + std::string(w[0]) + "' (milliseconds)");
            continue;
        }
        if (w.size() < 2)
        {
            fail("missing command");
            continue;
        }
        const auto cmd = w[1];
        using Kind = ControlEvent::Kind;
        auto& e = step.event;
        e.source = kSourceVirtual;
        e.timeMs = step.atMs;
        if (cmd == "quit")
        {
            step.op = ScriptStep::Op::Quit;
            r.steps.push_back(step);
        }
        else if (cmd == "cmd") // an app command (io::requestAppCommand): the rest of the line, words single-spaced
        {
            if (w.size() < 3)
            {
                fail("cmd needs a command");
                continue;
            }
            step.op = ScriptStep::Op::Command;
            for (std::size_t i = 2; i < w.size(); ++i)
                step.arg += (i > 2 ? " " : "") + std::string(w[i]);
            r.steps.push_back(step);
        }
        else if (cmd == "screenshot")
        {
            if (w.size() < 3)
            {
                fail("screenshot needs a file");
                continue;
            }
            step.op = ScriptStep::Op::Screenshot;
            step.arg = std::string(w[2]);
            r.steps.push_back(step);
        }
        else if (cmd == "press" || cmd == "release" || cmd == "turn" || cmd == "value")
        {
            if (w.size() < 3)
            {
                fail(std::string(cmd) + " needs a control");
                continue;
            }
            const auto c = scriptControl(w[2]);
            if (! c)
            {
                fail("unknown control '" + std::string(w[2]) + "'");
                continue;
            }
            int v = cmd == "press" ? 100 : 0;
            if ((cmd == "turn" || cmd == "value") && w.size() < 4)
            {
                fail(std::string(cmd) + " needs a value");
                continue;
            }
            if (w.size() >= 4 && ! number(w[3], v))
            {
                fail("bad value '" + std::string(w[3]) + "'");
                continue;
            }
            e.kind = cmd == "press"     ? Kind::Press
                   : cmd == "release" ? Kind::Release
                   : cmd == "turn"    ? Kind::Turn
                                      : Kind::Value;
            if (cmd == "press")
                v = std::clamp(v, 1, 127);
            e.control = *c;
            e.value = static_cast<std::int16_t>(std::clamp(v, -32768, 32767));
            r.steps.push_back(step);
        }
        else if (cmd == "strip") // the touch strip: `strip <0–127>` touches / moves, `strip off` lifts
        {
            int v = 0;
            const bool off = w.size() >= 3 && w[2] == "off";
            if (w.size() < 3 || (! off && ! number(w[2], v)))
            {
                fail("strip needs <0-127> or off");
                continue;
            }
            e.kind = off ? Kind::Release : Kind::Value;
            e.control = Control::Strip;
            e.value = static_cast<std::int16_t>(off ? 0 : std::clamp(v, 0, 127));
            r.steps.push_back(step);
        }
        else if (cmd == "touch" || cmd == "drag" || cmd == "release-touch" || cmd == "tap")
        {
            int x = lastX, y = lastY;
            const bool needPoint = cmd != "release-touch";
            if ((needPoint || w.size() >= 3) && (w.size() < 4 || ! number(w[2], x) || ! number(w[3], y)))
            {
                fail(std::string(cmd) + " needs <x> <y> (logical 800 x 480 points)");
                continue;
            }
            lastX = static_cast<std::int16_t>(std::clamp(x, 0, 799));
            lastY = static_cast<std::int16_t>(std::clamp(y, 0, 479));
            e.kind = Kind::Touch;
            e.control = Control::Screen;
            e.x = lastX;
            e.y = lastY;
            e.value = cmd == "touch" || cmd == "tap" ? 1 : cmd == "drag" ? 2 : 0;
            r.steps.push_back(step);
            if (cmd == "tap")
            {
                ScriptStep up = step;
                up.atMs = step.atMs + 50;
                up.event.timeMs = up.atMs;
                up.event.value = 0;
                r.steps.push_back(up);
            }
        }
        else
            fail("unknown command '" + std::string(cmd) + "'");
    }
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const ScriptStep& a, const ScriptStep& b) { return a.atMs < b.atMs; });
    return r;
}

void setInputScriptPath(const std::string& path) { scriptPath() = path; }
const std::string& inputScriptPath() { return scriptPath(); }

ScriptedInputSource::ScriptedInputSource(engine::TriggerSink* triggers, ScriptParseResult script, std::string origin)
    : triggers_(triggers), script_(std::move(script)), origin_(std::move(origin))
{
}

bool ScriptedInputSource::start(std::string& info)
{
    if (! script_.errors.empty())
    {
        info = origin_ + ": " + script_.errors.front()
             + (script_.errors.size() > 1 ? " (+" + std::to_string(script_.errors.size() - 1) + " more)" : "");
        script_.steps.clear(); // a broken script replays nothing
        return false;
    }
    const double end = script_.steps.empty() ? 0 : script_.steps.back().atMs;
    info = origin_ + ": " + std::to_string(script_.steps.size()) + " steps over "
         + std::to_string(static_cast<long>(end)) + " ms (time 0 = first poll)";
    return true;
}

void ScriptedInputSource::poll(std::vector<ControlEvent>& out) { pollAt(nowMs(), out); }

void ScriptedInputSource::pollAt(double now, std::vector<ControlEvent>& out)
{
    if (! started_)
    {
        started_ = true;
        t0_ = now;
    }
    const double t = now - t0_;
    while (next_ < script_.steps.size() && script_.steps[next_].atMs <= t)
    {
        const auto& s = script_.steps[next_++];
        switch (s.op)
        {
        case ScriptStep::Op::Quit: requestQuit(); break;
        case ScriptStep::Op::Screenshot: requestScreenshot(s.arg); break;
        case ScriptStep::Op::Command: requestAppCommand(s.arg); break;
        case ScriptStep::Op::Event:
        {
            ControlEvent e = s.event;
            e.timeMs = now; // the app's clock, like every other source
            if (e.kind == ControlEvent::Kind::Press && isPad(e.control) && triggers_ != nullptr) // UI-118
            {
                const int index = indexOf(e.control) - 1;
                triggers_->push({static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(e.value), 60,
                                 static_cast<std::int8_t>(index)});
                e.source = kSourcePanel; // sent direct, like a panel press: the pad controller must not post it again
            }
            out.push_back(e);
            break;
        }
        }
    }
}
} // namespace io

APP_REGISTER_CONTROL_SOURCE("script", &io::makeScripted)
