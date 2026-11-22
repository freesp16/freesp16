#include "io/controllers/profiles/ControllerHub.h"

#include "ProjectIdentity.h"
#include "io/controllers/ControlNames.h"
#include "io/controllers/profiles/Json.h"
#include "io/controllers/profiles/KeyRemap.h"
#include "io/midi/MidiSender.h"
#include "io/midi/MidiSettings.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace io::controllers
{
namespace
{
double steadyMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string_view roleName(PortRole r) noexcept
{
    return r == PortRole::Controller ? "controller" : r == PortRole::Instrument ? "instrument" : "auto";
}

// "USB MIDI 1" → "usb-midi-1" (a file-name and profile-id safe slug).
std::string slug(std::string_view name)
{
    std::string s;
    for (const char c : name)
    {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum)
            s += c;
        else if (c >= 'A' && c <= 'Z')
            s += static_cast<char>(c - 'A' + 'a');
        else if (! s.empty() && s.back() != '-')
            s += '-';
    }
    while (! s.empty() && s.back() == '-')
        s.pop_back();
    return s.empty() ? "port" : s.substr(0, 40);
}
} // namespace

ControllerHub::ControllerHub() = default;

ControllerHub::~ControllerHub()
{
    uninstall();
}

void ControllerHub::say(const std::string& line)
{
    if (log)
        log(line);
    else
    {
        std::printf("[sp16-mod] %s %s\n", identity::envPrefix, line.c_str());
        std::fflush(stdout);
    }
}

void ControllerHub::setDataDir(std::string dir)
{
    dataDir_ = std::move(dir);
    loadFiles();
}

void ControllerHub::setOverride(std::string spec)
{
    override_.clear();
    std::stringstream ss(spec);
    for (std::string item; std::getline(ss, item, ',');)
    {
        if (item.empty())
            continue;
        const auto colon = item.rfind(':');
        if (colon == std::string::npos)
            override_.push_back({item, ""});
        else
            override_.push_back({item.substr(0, colon), item.substr(colon + 1)});
    }
    for (auto& p : ports_)
        if (p->inSlot >= 0)
            activate(*p);
}

void ControllerHub::install()
{
    midi::setInputRouter(this);
    installed_ = true;
    lastReport_ = steadyMs();
}

void ControllerHub::uninstall()
{
    if (installed_ && midi::inputRouter() == this)
        midi::setInputRouter(nullptr);
    installed_ = false;
}

// ---- files ----

bool ControllerHub::writeFile(const std::string& path, const std::string& text)
{
    const std::string dir = dataDir_ + "/Controllers";
    ::mkdir(dataDir_.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0
                 && ::fsync(fileno(f)) == 0;
    std::fclose(f);
    if (! ok || std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

void ControllerHub::loadFiles()
{
    user_.clear();
    if (dataDir_.empty())
        return;
    const std::string dir = dataDir_ + "/Controllers";
    std::vector<std::string> names;
    if (DIR* d = ::opendir(dir.c_str()))
    {
        while (const dirent* e = ::readdir(d))
        {
            const std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0)
                names.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names)
    {
        const std::string text = readFile(dir + "/" + n);
        if (n == "settings.json")
        {
            auto parsed = json::parse(text);
            if (! parsed)
            {
                say("CTRL settings.json unreadable: " + parsed.error().detail);
                continue;
            }
            const auto& root = parsed.value();
            if (const auto* ports = root.get("ports"); ports != nullptr && ports->isObject())
                for (std::size_t k = 0; k < ports->keys.size(); ++k)
                {
                    const auto& o = ports->items[k];
                    auto& port = portNamed(ports->keys[k]);
                    auto& s = port.settings;
                    auto num = [&](std::string_view key, int def) {
                        const auto* v = o.get(key);
                        return v != nullptr && v->isNumber() ? static_cast<int>(v->number) : def;
                    };
                    if (const auto* r = o.get("role"); r != nullptr && r->isString())
                        s.role = r->string == "controller" ? PortRole::Controller
                               : r->string == "instrument" ? PortRole::Instrument
                                                           : PortRole::Auto;
                    if (const auto* p = o.get("profile"); p != nullptr && p->isString())
                        s.profile = p->string;
                    s.response.shape = static_cast<PadCurveShape>(std::clamp(num("curve", 0), -3, 100));
                    if (s.response.shape > PadCurveShape::Soft3 && s.response.shape != PadCurveShape::Fixed)
                        s.response.shape = PadCurveShape::Linear;
                    s.response.threshold = std::clamp(num("threshold", 1), 1, 16);
                    s.response.sensitivity = std::clamp(num("sensitivity", 8), 1, 16);
                    s.response.fixedVelocity = std::clamp(num("fixed", 100), 1, 127);
                    if (const auto* t = o.get("trim"); t != nullptr && t->isArray())
                        for (std::size_t i = 0; i < t->items.size() && i < 16; ++i)
                            if (t->items[i].isNumber())
                                s.trim[i] = static_cast<std::int8_t>(std::clamp(static_cast<int>(t->items[i].number), -20, 20));
                }
            if (const auto* k = root.get("keys"); k != nullptr && k->isArray())
            {
                std::vector<KeyBinding> keysOut;
                for (const auto& e : k->items)
                    if (e.isArray() && e.items.size() == 3 && e.items[0].isNumber() && e.items[1].isNumber()
                        && e.items[2].isString())
                        if (const auto c = controlFromName(e.items[2].string))
                            keysOut.push_back({static_cast<int>(e.items[0].number),
                                               static_cast<std::uint8_t>(e.items[1].number), *c});
                keyRemap().setOverrides(std::move(keysOut));
            }
            keysSaved_ = keyRemap().version();
            continue;
        }
        auto parsed = parseProfile(text);
        if (! parsed)
        {
            say("CTRL profile " + n + " skipped: " + parsed.error().detail);
            continue;
        }
        const bool clash = std::any_of(builtinProfiles().begin(), builtinProfiles().end(),
                                       [&](const LoadedProfile& b) { return b.id == parsed.value().id; });
        if (clash)
        {
            say("CTRL profile " + n + " skipped: id " + parsed.value().id + " is a built-in profile");
            continue;
        }
        user_.push_back(std::move(parsed.value()));
    }
}

void ControllerHub::saveSettings()
{
    if (dataDir_.empty())
        return;
    std::string s = "{\n  \"kind\": \"controller-settings\",\n  \"schema\": 1,\n  \"ports\": {";
    bool first = true;
    for (const auto& p : ports_)
    {
        const auto& st = p->settings;
        s += first ? "\n    " : ",\n    ";
        first = false;
        s += json::quote(p->name) + ": {\"role\": " + json::quote(roleName(st.role)) + ", \"profile\": "
           + json::quote(st.profile) + ", \"curve\": " + std::to_string(static_cast<int>(st.response.shape))
           + ", \"threshold\": " + std::to_string(st.response.threshold)
           + ", \"sensitivity\": " + std::to_string(st.response.sensitivity)
           + ", \"fixed\": " + std::to_string(st.response.fixedVelocity) + ", \"trim\": [";
        for (int i = 0; i < 16; ++i)
            s += (i == 0 ? "" : ", ") + std::to_string(st.trim[static_cast<std::size_t>(i)]);
        s += "]}";
    }
    s += first ? "},\n  \"keys\": [" : "\n  },\n  \"keys\": [";
    first = true;
    for (const auto& k : keyRemap().overrides())
    {
        s += (first ? "" : ", ") + std::string("[") + std::to_string(k.code) + ", " + std::to_string(k.cls) + ", "
           + json::quote(nameOf(k.control)) + "]";
        first = false;
    }
    s += "]\n}\n";
    if (! writeFile(dataDir_ + "/Controllers/settings.json", s))
        say("CTRL settings.json not written (" + dataDir_ + "/Controllers)");
    keysSaved_ = keyRemap().version();
}

// ---- ports ----

ControllerHub::Port& ControllerHub::portNamed(std::string_view name)
{
    for (auto& p : ports_)
        if (p->name == name)
            return *p;
    ports_.push_back(std::make_unique<Port>());
    ports_.back()->name = std::string(name);
    return *ports_.back();
}

const ControllerHub::Port* ControllerHub::findPort(std::string_view name) const
{
    for (const auto& p : ports_)
        if (p->name == name)
            return p.get();
    return nullptr;
}

std::vector<const LoadedProfile*> ControllerHub::profiles() const
{
    std::vector<const LoadedProfile*> out;
    for (const auto& p : builtinProfiles())
        out.push_back(&p);
    for (const auto& p : user_)
        out.push_back(&p);
    return out;
}

const LoadedProfile* ControllerHub::profile(std::string_view id) const
{
    for (const auto* p : profiles())
        if (p->id == id)
            return p;
    return nullptr;
}

bool ControllerHub::isController(const Port& p) const
{
    for (const auto& [glob, id] : override_)
        if (matchesPort(glob, p.name))
            return true;
    if (p.settings.role != PortRole::Auto)
        return p.settings.role == PortRole::Controller;
    for (const auto* prof : profiles())
        if (! prof->portPattern.empty() && ! prof->fallback() && matchesPort(prof->portPattern, p.name))
            return true;
    return false;
}

const LoadedProfile& ControllerHub::profileFor(const Port& p) const
{
    for (const auto& [glob, id] : override_)
        if (matchesPort(glob, p.name) && ! id.empty())
            if (const auto* prof = profile(id))
                return *prof;
    if (! p.settings.profile.empty())
        if (const auto* prof = profile(p.settings.profile))
            return *prof;
    const auto all = profiles();
    for (const auto* prof : all)
        if (! prof->portPattern.empty() && ! prof->fallback() && matchesPort(prof->portPattern, p.name))
            return *prof;
    for (const auto* prof : all)
        if (prof->id == kGenericProfileId)
            return *prof;
    return *all.front();
}

void ControllerHub::activate(Port& p)
{
    if (p.inSlot < 0 || p.inSlot >= kPorts)
        return;
    auto& slot = active_[static_cast<std::size_t>(p.inSlot)];
    const bool want = isController(p);
    if (! want)
    {
        if (p.source)
        {
            slot.store(nullptr, std::memory_order_release);
            p.retired.push_back(std::move(p.source)); // the reader thread may still hold it: freed when the port closes
            say("CTRL port " + std::to_string(p.inSlot + 1) + " " + p.name + " instrument (MIDI input)");
        }
        return;
    }
    const auto& prof = profileFor(p);
    if (p.source && p.source->profileId() == prof.id)
        return;
    auto src = std::make_unique<MidiControlSource>(prof, p.inSlot, p.name, triggers_);
    src->setResponse(p.settings.response);
    for (int i = 0; i < 16; ++i)
        src->setTrim(i, p.settings.trim[static_cast<std::size_t>(i)]);
    if (p.source)
        p.retired.push_back(std::move(p.source));
    p.source = std::move(src);
    wireLamps(p);
    slot.store(p.source.get(), std::memory_order_release);
    say("CTRL port " + std::to_string(p.inSlot + 1) + " " + p.name + " controller profile=" + prof.id
        + (p.source->hasLamps() ? " lamps=" + std::string(p.outSlot >= 0 ? "out " + std::to_string(p.outSlot + 1) : "no-output")
                                : " lamps=none"));
}

void ControllerHub::wireLamps(Port& p)
{
    if (! p.source)
        return;
    if (p.sender != nullptr && p.outSlot >= 0)
    {
        auto* sender = p.sender;
        const int out = p.outSlot;
        p.source->setSend([sender, out](const std::uint8_t* b, int n) { return sender->sendNow(out, b, n); });
    }
    else
        p.source->setSend({});
}

void ControllerHub::inputOpened(int slot, std::string_view name)
{
    if (slot < 0 || slot >= kPorts)
        return;
    auto& p = portNamed(name);
    if (p.inSlot == slot && p.source)
        return; // already told (a late router learns about open ports again)
    p.inSlot = slot;
    activate(p);
}

void ControllerHub::inputClosed(int slot)
{
    if (slot < 0 || slot >= kPorts)
        return;
    active_[static_cast<std::size_t>(slot)].store(nullptr, std::memory_order_release);
    for (auto& p : ports_)
        if (p->inSlot == slot)
        {
            if (p->source)
                say("CTRL port " + std::to_string(slot + 1) + " " + p->name + " disconnected");
            p->source.reset(); // the reader thread has ended
            p->retired.clear();
            p->inSlot = -1;
            if (learnPort_ == p->name)
                cancelLearn();
        }
}

void ControllerHub::outputOpened(int slot, std::string_view name, midi::MidiSender& sender)
{
    auto& p = portNamed(name);
    const bool news = p.outSlot != slot || p.sender != &sender;
    p.outSlot = slot;
    p.sender = &sender;
    wireLamps(p);
    if (news && p.source && p.source->hasLamps())
        say("CTRL port " + p.name + " lamps=out " + std::to_string(slot + 1));
}

void ControllerHub::outputClosed(int slot)
{
    for (auto& p : ports_)
        if (p->outSlot == slot)
        {
            p->outSlot = -1;
            p->sender = nullptr;
            wireLamps(*p);
        }
}

void ControllerHub::rawBytes(int slot, const std::uint8_t* bytes, int size, std::int64_t hostNs) noexcept
{
    if (slot < 0 || slot >= kPorts)
        return;
    if (auto* s = active_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire))
        s->onRawBytes(bytes, size, static_cast<double>(hostNs) * 1.0e-6);
}

bool ControllerHub::message(int slot, const core::midi::MidiEvent& e) noexcept
{
    if (slot < 0 || slot >= kPorts)
        return false;
    auto* s = active_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
    return s != nullptr && s->onMidi(e.bytes, e.size, static_cast<double>(e.hostTimeNs) * 1.0e-6);
}

// ---- frame ----

void ControllerHub::poll(std::vector<ControlEvent>& out)
{
    for (auto& p : ports_)
        if (p->source)
            p->source->poll(out);
    pollLearn();
    if (keyRemap().version() != keysSaved_)
        saveSettings(); // KEYBOARD MAP edits (UI-120: stored in settings)
    const double now = steadyMs();
    if (installed_ && now - lastReport_ >= 10000.0)
    {
        lastReport_ = now;
        report();
    }
}

void ControllerHub::pollLearn()
{
    if (learn_ != LearnState::Waiting)
        return;
    if (const auto* p = findPort(learnPort_); p != nullptr && p->source)
        if (p->source->takeLearned(learned_))
        {
            learn_ = LearnState::Captured;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%02X %d", learned_.status, learned_.number);
            say("CTRL learn " + learnPort_ + " captured " + buf);
        }
}

void ControllerHub::setLamp(Control c, std::uint32_t rgb, std::uint8_t level)
{
    for (auto& p : ports_)
        if (p->source)
            p->source->setLamp(c, rgb, level);
}

void ControllerHub::flush()
{
    for (auto& p : ports_)
        if (p->source)
            p->source->flush();
}

void ControllerHub::report()
{
    for (const auto& p : ports_)
        if (p->source)
        {
            const auto s = p->source->stats();
            char buf[300];
            std::snprintf(buf, sizeof buf,
                          "CTRL stats port=%s profile=%s in=%llu consumed=%llu events=%llu triggers=%llu lamps=%llu "
                          "dropped=%llu",
                          p->name.c_str(), p->source->profileId().c_str(), static_cast<unsigned long long>(s.messages),
                          static_cast<unsigned long long>(s.consumed), static_cast<unsigned long long>(s.events),
                          static_cast<unsigned long long>(s.triggers), static_cast<unsigned long long>(s.lampsSent),
                          static_cast<unsigned long long>(s.dropped));
            say(buf);
        }
}

// ---- the CONTROLLERS page ----

std::vector<ControllerPort> ControllerHub::ports() const
{
    std::vector<ControllerPort> out;
    for (const auto& p : ports_)
    {
        ControllerPort v;
        v.name = p->name;
        v.connected = p->inSlot >= 0;
        v.controller = isController(*p);
        v.settings = p->settings;
        const auto& prof = profileFor(*p);
        v.profileId = p->source ? p->source->profileId() : prof.id;
        v.profileName = prof.name;
        v.hasLamps = ! prof.lamps.empty();
        v.lastVelocity = p->source ? p->source->lastVelocity() : 0;
        out.push_back(std::move(v));
    }
    return out;
}

int ControllerHub::connectedCount() const noexcept
{
    int n = 0;
    for (const auto& p : ports_)
        n += p->source ? 1 : 0;
    return n;
}

MidiControlSource* ControllerHub::source(std::string_view port) const
{
    const auto* p = findPort(port);
    return p != nullptr ? p->source.get() : nullptr;
}

void ControllerHub::setRole(std::string_view port, PortRole r)
{
    auto& p = portNamed(port);
    if (p.settings.role == r)
        return;
    p.settings.role = r;
    say("CTRL port " + p.name + " role=" + std::string(roleName(r)));
    activate(p);
    saveSettings();
}

void ControllerHub::setProfile(std::string_view port, std::string id)
{
    auto& p = portNamed(port);
    p.settings.profile = std::move(id);
    activate(p);
    saveSettings();
}

void ControllerHub::setResponse(std::string_view port, const PadResponse& r)
{
    auto& p = portNamed(port);
    p.settings.response = r;
    if (p.source)
        p.source->setResponse(r);
    saveSettings();
}

void ControllerHub::setTrim(std::string_view port, int pad0, int trim)
{
    if (pad0 < 0 || pad0 > 15)
        return;
    auto& p = portNamed(port);
    p.settings.trim[static_cast<std::size_t>(pad0)] = static_cast<std::int8_t>(std::clamp(trim, -20, 20));
    if (p.source)
        p.source->setTrim(pad0, trim);
    saveSettings();
}

bool ControllerHub::startLearn(std::string_view port)
{
    cancelLearn();
    auto& p = portNamed(port);
    if (! p.source)
        return false;
    p.source->armLearn();
    learnPort_ = p.name;
    learn_ = LearnState::Waiting;
    learned_ = {};
    say("CTRL learn " + p.name + " waiting");
    return true;
}

void ControllerHub::cancelLearn()
{
    if (const auto* p = findPort(learnPort_); p != nullptr && p->source)
        p->source->cancelLearn();
    learn_ = LearnState::Idle;
}

bool ControllerHub::assignLearned(Control c, std::string& why)
{
    if (learn_ != LearnState::Captured)
    {
        why = "nothing captured";
        return false;
    }
    auto& p = portNamed(learnPort_);
    const auto base = profileFor(p); // copy
    LoadedProfile user = base;
    user.builtin = false;
    if (base.builtin || base.id.rfind("user-", 0) != 0)
    {
        user.id = "user-" + slug(p.name);
        user.name = p.name + " (learned)";
        user.portPattern = p.name; // matched to this port by name from now on
    }
    // The captured message → a binding with the encoding a control of that kind takes from it.
    MidiBinding b;
    const auto st = learned_.status;
    const int hi = st & 0xF0;
    b.channel = static_cast<std::uint8_t>(st < 0xF0 ? (st & 0x0F) + 1 : 0);
    b.control = c;
    const auto kind = controlInfo(c).kind;
    if (st >= 0xFA && st <= 0xFC)
    {
        b.status = status::realtime;
        b.number = st;
        b.enc = Encoding::Realtime;
    }
    else if (st == 0xF0)
    {
        b.status = status::mmc;
        b.number = learned_.number;
        b.enc = Encoding::Mmc;
    }
    else if (hi == 0x90)
    {
        b.status = status::note;
        b.number = learned_.number;
        b.enc = Encoding::Note;
        b.aftertouch = kind == ControlKind::Pad;
    }
    else if (hi == 0xB0)
    {
        b.status = status::cc;
        b.number = learned_.number;
        // An endless encoder sends small steps around 0 / 64 (a guess from the first value: 1–15 or 113–127 =
        // two's complement, 49–79 but not 64 … = binary offset); anything else is absolute.
        const int v = learned_.value;
        b.enc = kind == ControlKind::Encoder && ((v >= 1 && v <= 15) || v >= 113) ? Encoding::TwosComplement
              : kind == ControlKind::Encoder && v >= 49 && v <= 79 && v != 64 ? Encoding::BinaryOffset
                                                                              : Encoding::Absolute;
    }
    else if (hi == 0xE0)
    {
        b.status = status::pitchBend;
        b.enc = Encoding::PitchBend14;
    }
    else if (hi == 0xD0)
    {
        b.status = status::pressure;
        b.enc = Encoding::Absolute;
    }
    else
    {
        why = "that message cannot be learned";
        return false;
    }
    // Replace any binding of the same message (and, for poly-AT pads, of the same note).
    user.in.erase(std::remove_if(user.in.begin(), user.in.end(),
                                 [&](const MidiBinding& o) {
                                     return o.status == b.status && o.number == b.number
                                         && (o.channel == b.channel || o.channel == 0 || b.channel == 0);
                                 }),
                  user.in.end());
    user.in.push_back(b);
    // Validate through the same parser a file goes through.
    auto checked = parseProfile(toJson(user));
    if (! checked)
    {
        why = checked.error().detail;
        return false;
    }
    if (! dataDir_.empty() && ! writeFile(dataDir_ + "/Controllers/" + user.id + ".json", toJson(user)))
    {
        why = "the profile could not be saved";
        return false;
    }
    bool replaced = false;
    for (auto& u : user_)
        if (u.id == user.id)
        {
            u = checked.value();
            replaced = true;
        }
    if (! replaced)
        user_.push_back(checked.value());
    say("CTRL learn " + p.name + " " + std::string(nameOf(c)) + " saved profile=" + user.id);
    p.settings.profile = user.id;
    // Force a new source with the updated bindings.
    if (p.source)
    {
        active_[static_cast<std::size_t>(p.inSlot)].store(nullptr, std::memory_order_release);
        p.retired.push_back(std::move(p.source));
    }
    activate(p);
    saveSettings();
    learn_ = LearnState::Idle;
    return true;
}

// ---- the app's instance and its control source ----

ControllerHub& hub()
{
    static ControllerHub* instance = new ControllerHub(); // process lifetime: reader threads may call it to the end
    return *instance;
}

int connectedControllers() noexcept
{
    return hub().connectedCount();
}

namespace
{
// The registered "controllers" source: the hub's frame calls (UI thread) and its lamps.
class HubSource final : public ControlSource, public LampSink
{
public:
    explicit HubSource(engine::TriggerSink* t) : triggers_(t) {}
    std::string_view id() const override { return "controllers"; }
    bool start(std::string& info) override
    {
        auto& h = hub();
        h.setTriggers(triggers_);
        h.setOverride(midi::setting("controllers"));
        h.install();
        info = std::to_string(h.profiles().size()) + " profiles (" + std::to_string(builtinProfiles().size())
             + " built-in), data " + (h.dataDir().empty() ? std::string("none") : h.dataDir() + "/Controllers");
        return true;
    }
    void stop() override
    {
        hub().report();
        hub().uninstall();
    }
    void poll(std::vector<ControlEvent>& out) override { hub().poll(out); }
    void setLamp(Control c, std::uint32_t rgb, std::uint8_t level) override { hub().setLamp(c, rgb, level); }
    void flush() override { hub().flush(); }

private:
    engine::TriggerSink* triggers_;
};

std::unique_ptr<ControlSource> makeHubSource(engine::TriggerSink* triggers)
{
    return std::make_unique<HubSource>(triggers);
}
} // namespace
} // namespace io::controllers

APP_REGISTER_CONTROL_SOURCE("controllers", &io::controllers::makeHubSource)
