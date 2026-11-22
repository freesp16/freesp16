#include "app/ext/Extensions.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/ext/ExtCommands.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/params/ParamRegistry.h"
#include "services/ext/ExtFlags.h"
#include "services/interop/sp16export/Sp16Export.h"

#include <cstdio>

namespace app::ext
{
namespace
{
namespace flags = services::ext;
namespace ml = services::midilearn;
constexpr const char* kToggleKey = "native.ext.trk.toggle";
constexpr std::string_view kToggleParam = "ext.toggle.track"; // = engine::trigger::ext::ToggleLatch::kParamId

core::ParamId paramId(std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }

// The live engine's hub (the first engine constructed claims it, core/util/midi), null without one.
core::midi::MidiHub* hub() { return core::midi::liveHub(); }

// The overlay lives as long as the process: the engine may still read it after this object is gone.
core::midi::CcTable& overlayTable()
{
    static core::midi::CcTable t;
    return t;
}

bool toggleOf(const core::model::Track& t)
{
    const auto* e = t.extras.find(kToggleKey);
    return e != nullptr && e->value == "true";
}

std::string num(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
} // namespace

Extensions::Extensions(ExtEnv env) : env_(std::move(env))
{
    if (env_.preference) // the flags from the preferences (UTILITY's SIXTEEN EXTENSIONS and EXTENSIONS rows)
        flags::flags().setSource([pref = env_.preference](std::string_view key) -> std::optional<float> {
            if (auto v = flags::environmentValue(key))
                return v;
            return pref(key);
        });
}

Extensions::~Extensions()
{
    publishOverlay(false);
    if (env_.preference)
        flags::flags().setSource(&flags::environmentValue);
}

bool Extensions::hasProject() const { return env_.bus != nullptr && core::commands::hasProject(*env_.bus); }
const core::model::Project& Extensions::project() const { return core::commands::project(*env_.bus); }

void Extensions::log(const std::string& line) const
{
    if (env_.log)
        env_.log(line);
}

// ------------------------------------------------------------------------------------------------- E11 Toggle
void Extensions::syncToggles(const core::model::Project& p)
{
    if (env_.bridge == nullptr)
        return;
    const auto id = paramId(kToggleParam);
    if (! id.valid())
        return;
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
        {
            const bool want = toggleOn_ && toggleOf(p.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)]);
            const auto bit = static_cast<std::uint16_t>(1u << t);
            const bool posted = (postedToggles_[static_cast<std::size_t>(s)] & bit) != 0;
            if (want == posted)
                continue;
            const auto scope = static_cast<std::uint16_t>((s << 4) | t);
            if (! env_.bridge->post(engine::ParamChange {id, scope, want ? 1.0f : 0.0f, 0}))
                continue; // the FIFO is full: tried again at the next change / frame
            postedToggles_[static_cast<std::size_t>(s)] =
                static_cast<std::uint16_t>(want ? postedToggles_[static_cast<std::size_t>(s)] | bit
                                                : postedToggles_[static_cast<std::size_t>(s)] & ~bit);
        }
}

// ---------------------------------------------------------------------------------------------- E4 MIDI learn
void Extensions::publishOverlay(bool on)
{
    auto* h = hub();
    if (h == nullptr)
        return;
    auto& overlay = overlayTable();
    const auto* current = h->ccTable();
    if (on)
    {
        if (current == nullptr || current == &overlay)
            return; // no fixed table yet (MIDI off / not started), or already published
        fixed_ = current;
        ml::buildOverlay(*fixed_, paramId(ml::kCarrierId), overlay);
        h->setCcTable(&overlay);
    }
    else if (current == &overlay)
    {
        h->setCcTable(fixed_);
        fixed_ = nullptr;
    }
}

void Extensions::syncLearn(const core::model::Project& p)
{
    std::string warnings;
    learn_ = ml::LearnMap::load(p, core::builtinParams(), &warnings);
    if (! warnings.empty() && learnOn_)
        log("EXT LEARN load: " + warnings);
}

void Extensions::storeLearnMap()
{
    if (! hasProject())
        return;
    const std::optional<std::string> json =
        learn_.empty() ? std::nullopt : std::optional<std::string>(learn_.toJson(core::builtinParams()));
    env_.bus->execute(core::commands::ext::setExtra(-1, -1, std::string(ml::kExtrasKey), json, "MIDI LEARN"));
}

bool Extensions::onMidiUi(const core::midi::UiEvent& e)
{
    if (! learnOn_ || e.kind != core::midi::UiEvent::Kind::Param || ! hasProject())
        return false;
    const auto carrier = paramId(ml::kCarrierId);
    if (! carrier.valid() || e.param != carrier.value)
        return false;
    const auto m = ml::decodeCarrier(e, carrier, project().midi);
    if (! m)
        return true;
    const auto& reg = core::builtinParams();
    if (session_.armed())
    {
        // The hub does not say which port a CC came from; the controller hub's `controller` ports never reach the
        // engine's CC path in this build, so every carrier CC counts as a performance port.
        if (const auto b = session_.capture(*m, false))
        {
            auto ok = learn_.add(*b, reg, fixed_);
            if (ok)
                storeLearnMap();
            const auto id = reg[b->target].id;
            log(std::string(ok ? "EXT LEARN bound" : "EXT LEARN refused") + " ch=" + std::to_string(b->channel) +
                " cc=" + std::to_string(b->number) + " param=" + std::string(id) + " scope=" + std::to_string(b->scope) +
                (ok ? std::string() : " (" + ok.error().message + ")"));
        }
        return true;
    }
    std::vector<ml::Applied> out;
    learn_.apply(*m, reg, out);
    for (const auto& a : out)
    {
        if (env_.setRemote)
            env_.setRemote(*a.spec, a.scope, a.value);
        if (++learnApplied_ <= 16)
            log("EXT MIDI " + std::string(a.spec->id) + " scope=" + std::to_string(a.scope) + " value=" + num(a.value) +
                " ch=" + std::to_string(m->channel) + " cc=" + std::to_string(m->number) + " cc_value=" +
                std::to_string(m->value));
    }
    return true;
}

// --------------------------------------------------------------------------------------------- E12 strip record
void Extensions::handle(const io::ControlEvent& e)
{
    using Kind = io::ControlEvent::Kind;
    if (! io::isPad(e.control) || (e.kind != Kind::Press && e.kind != Kind::Release))
        return;
    const int pad = io::indexOf(e.control) - 1;
    if (pad < 0 || pad > 15)
        return;
    const auto bit = static_cast<std::uint16_t>(1u << pad);
    heldPads_ = static_cast<std::uint16_t>(e.kind == Kind::Press ? heldPads_ | bit : heldPads_ & ~bit);
    // The track a held pad plays (the router's pad routing): PLAY = pad n, SLICE / SCALE = the selected track.
    const int mode = env_.padMode ? env_.padMode() : 0;
    heldTracks_ = 0;
    for (int k = 0; k < 16; ++k)
        if ((heldPads_ >> k & 1u) != 0)
        {
            if (mode == 0)
                heldTracks_ = static_cast<std::uint16_t>(heldTracks_ | (1u << k));
            else if ((mode == 3 || mode == 4) && env_.selectedTrack)
                heldTracks_ = static_cast<std::uint16_t>(heldTracks_ | (1u << (env_.selectedTrack() & 15)));
        }
}

std::uint16_t Extensions::stripTracks() const
{
    int applyTo = 0; // ribbon.applyTo: 0 HELD PADS (the reference), 1 SELECTED TRACK, 2 ALL TRACKS
    if (const auto* spec = core::builtinParams().find("ribbon.applyTo"); spec != nullptr && env_.value)
        applyTo = static_cast<int>(env_.value(*spec, 0) + 0.5f);
    if (applyTo == 1)
        return env_.selectedTrack ? static_cast<std::uint16_t>(1u << (env_.selectedTrack() & 15)) : 0;
    if (applyTo == 2)
        return 0xFFFF;
    return heldTracks_;
}

// ---------------------------------------------------------------------------------------------- hooks
void Extensions::onChange(const core::ChangeSet& changes, const core::model::Project& p)
{
    if (! toggleOn_ && ! learnOn_)
        return;
    bool toggles = false, learn = false;
    for (const auto& path : changes.paths())
    {
        toggles = toggles || path.find(".extras.native.ext.trk.toggle") != std::string::npos;
        learn = learn || path == "extras." + std::string(ml::kExtrasKey);
    }
    if (toggles)
        syncToggles(p);
    if (learn)
        syncLearn(p);
}

void Extensions::onProjectReplaced(const core::model::Project& p)
{
    postedToggles_.fill(0); // the engine's toggles belong to the project it had: re-posted from this one
    syncToggles(p);
    syncLearn(p);
}

void Extensions::frame(double, const engine::PlayheadFeed& ph, int stripMode, bool stripContact, int stripPosition)
{
    const bool toggle = flags::flags().isOn(flags::kToggle);
    if (toggle != toggleOn_)
    {
        toggleOn_ = toggle;
        if (hasProject())
            syncToggles(project()); // on: the model's toggles; off: every posted one back to 0
    }
    const bool learn = flags::flags().isOn(flags::kMidiLearn);
    if (learn != learnOn_)
    {
        learnOn_ = learn;
        if (! learn)
            session_.cancel();
        else if (hasProject())
            syncLearn(project());
    }
    if (learnOn_)
        publishOverlay(true); // also once the MIDI source has published its fixed table
    else if (fixed_ != nullptr)
        publishOverlay(false);

    if (! flags::flags().isOn(flags::kStripRecord) || ! hasProject())
        return;
    ui::input::ext::StripRecordState s;
    s.recording = ph.playing && env_.recArmed && env_.recArmed();
    s.scene = ph.scene & 15;
    s.pattern = ph.pattern & 15;
    s.step = ph.step;
    s.mode = stripMode;
    s.contact = stripContact;
    s.position = stripPosition;
    s.tracks = stripTracks();
    writes_.clear();
    strip_.frame(s, project().ribbon, writes_);
    for (const auto& w : writes_)
    {
        env_.bus->execute(core::commands::sequencer::setLock(w.scene, w.pattern, w.track, w.step, w.slot, w.value));
        if (++stripWrites_ <= 16)
            log("EXT STRIP lock " + std::string(w.spec != nullptr ? w.spec->id : std::string_view("?")) +
                " scene=" + std::to_string(w.scene + 1) + " pattern=" + std::to_string(w.pattern + 1) + " track=" +
                std::to_string(w.track + 1) + " step=" + std::to_string(w.step + 1) + " value=" + num(w.value));
    }
}

// ---------------------------------------------------------------------------------------------- `cmd ext-*`
bool Extensions::command(const std::string& cmd, std::istringstream& in)
{
    auto& f = flags::flags();
    auto onOff = [](const std::string& s) { return s == "on" || s == "1" || s == "true"; };
    if (cmd == "ext-flag")
    {
        std::string flag, value;
        in >> flag >> value;
        f.set(flag, onOff(value));
        log("EXT FLAG " + flag + " " + (onOff(value) ? "on" : "off") + " (effective " + (f.isOn(flag) ? "on" : "off") + ")");
        return true;
    }
    if (cmd == "ext-state")
    {
        std::string toggles;
        if (hasProject())
            for (int s = 0; s < 16; ++s)
                for (int t = 0; t < 16; ++t)
                    if (toggleOf(project().scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)]))
                        toggles += (toggles.empty() ? "" : ",") + std::to_string(s + 1) + "." + std::to_string(t + 1);
        log(std::string("EXT STATE enabled=") + (f.isOn(flags::kMaster) ? "1" : "0") + " toggle=" +
            (f.isOn(flags::kToggle) ? "1" : "0") + " learn=" + (f.isOn(flags::kMidiLearn) ? "1" : "0") +
            " export=" + (f.isOn(flags::kExport) ? "1" : "0") + " strip=" + (f.isOn(flags::kStripRecord) ? "1" : "0") +
            " toggles=" + (toggles.empty() ? "-" : toggles) + " bindings=" + std::to_string(learn_.bindings().size()) +
            " overlay=" + (fixed_ != nullptr ? "1" : "0"));
        return true;
    }
    if (cmd == "ext-toggle")
    {
        int track = 0;
        std::string value;
        in >> track >> value;
        if (! f.isOn(flags::kToggle) || ! hasProject() || track < 1 || track > 16)
        {
            log("EXT TOGGLE refused (ext.trk.toggle off, no project or no track 1-16)");
            return true;
        }
        const int scene = project().settings.currentScene & 15;
        const bool on = onOff(value);
        env_.bus->execute(core::commands::ext::setExtra(scene, track - 1, kToggleKey,
                                                        on ? std::optional<std::string>("true") : std::nullopt, "TOGGLE"));
        log("EXT TOGGLE scene=" + std::to_string(scene + 1) + " track=" + std::to_string(track) + " " + (on ? "on" : "off"));
        return true;
    }
    if (cmd == "ext-learn")
    {
        std::string id;
        int track = 0;
        float lo = 0, hi = 1;
        in >> id >> track;
        if (! (in >> lo >> hi))
            lo = 0, hi = 1;
        const auto* spec = core::builtinParams().find(id);
        if (! f.isOn(flags::kMidiLearn) || ! hasProject() || spec == nullptr || ! ml::learnable(*spec))
        {
            log("EXT LEARN refused " + id + " (ext.midi.learn off, no project or not a learnable parameter)");
            return true;
        }
        const int scene = project().settings.currentScene & 15;
        const int t = (track >= 1 && track <= 16 ? track : (env_.selectedTrack ? env_.selectedTrack() + 1 : 1)) - 1;
        std::uint16_t scope = 0;
        if (spec->scope == core::ParamScope::Track)
            scope = static_cast<std::uint16_t>((scene << 4) | t);
        else if (spec->scope == core::ParamScope::Scene || spec->scope == core::ParamScope::SceneOverrideElseProject)
            scope = static_cast<std::uint16_t>(scene);
        session_.arm(*core::builtinParams().idOf(id), scope, lo, hi);
        log("EXT LEARN armed " + id + " scope=" + std::to_string(scope) + " (move a CC)");
        return true;
    }
    if (cmd == "ext-unlearn")
    {
        int channel = 0, number = 0;
        in >> channel >> number;
        const bool ok = learn_.remove(static_cast<std::uint8_t>(channel), ml::kStatusCc, static_cast<std::uint8_t>(number));
        if (ok)
            storeLearnMap();
        log("EXT UNLEARN ch=" + std::to_string(channel) + " cc=" + std::to_string(number) + (ok ? " ok" : " none"));
        return true;
    }
    if (cmd == "ext-export")
    {
        std::string folder, name;
        in >> folder >> name;
        if (! f.isOn(flags::kExport) || ! hasProject() || ! juce::File::isAbsolutePath(folder))
        {
            log("EXPORT refused (ext.interop.export off, no project or no absolute folder)");
            return true;
        }
        services::interop::sp16::ExportOptions o;
        o.name = name;
        o.locate = [this](const core::model::SampleRef& ref) {
            if (juce::File::isAbsolutePath(ref.path))
                return juce::File(ref.path);
            if (env_.resolveSample)
                if (auto file = env_.resolveSample(ref.path))
                    return *file;
            return juce::File();
        };
        services::interop::sp16::ExportReport report;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        auto r = services::interop::sp16::exportProject(project(), juce::File(folder), o, report);
        for (const auto& line : report.lines())
            log("EXPORT " + line);
        log(std::string("EXPORT ") + (r ? "ok file=" + report.projectFile.getFullPathName().toStdString()
                                         : "FAILED " + r.error().message) +
            " ms=" + std::to_string(static_cast<long>(juce::Time::getMillisecondCounterHiRes() - t0)));
        return true;
    }
    return false;
}
} // namespace app::ext
