// Export for the reference (FMT-044, LIB-041): generated → import → export → import is equal
// at model level; the exported file decodes with tools/prj-dump/prj-dump.py --roundtrip; a second export of the
// re-import is byte-identical; samples become 44.1 kHz WAV ≤ 64 s with sidecars; the export never overwrites, never
// writes onto an instrument disk layout and is off unless ext.enabled + ext.interop.export are on.
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/params/ParamRegistry.h"
#include "services/ext/ExtFlags.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/interop/sp16export/Sp16Export.h"
#include "services/store/CodecRegistry.h"
#include "tests/unit/interop/Fixtures.h"

#include "services/library/AudioFile.h"
#include "services/render/AudioWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <sstream>

using namespace services::interop::sp16;
namespace m = core::model;
namespace store = services::store;

namespace
{
juce::File tempDir(const std::string& name)
{
    auto d = interop_test::temp().dir().getChildFile("ext-export").getChildFile(name);
    d.getParentDirectory().createDirectory();
    d.deleteRecursively();
    return d;
}

std::unique_ptr<m::Project> importFile(const juce::File& prj)
{
    ImportOptions o;
    o.verifyRoundTrip = false;
    store::ImportReport report;
    auto p = importProjectFile(prj, o, report);
    INFO((p ? std::string() : p.error().message));
    REQUIRE(p);
    return std::move(p.value());
}

std::string extrasText(const m::Extras& e)
{
    std::string s;
    for (const auto& x : e.entries)
        s += "  " + x.key + " = " + x.value.substr(0, 120) + "\n";
    return s;
}

// The first difference between two projects (a readable path), "" when equal. Sample pool paths are compared by file
// name (an export lives in another folder).
std::string firstDifference(const m::Project& a, const m::Project& b)
{
    if (! (a.settings == b.settings))
        return "settings\n" + extrasText(a.settings.extras) + "vs\n" + extrasText(b.settings.extras);
    for (int s = 0; s < 16; ++s)
    {
        const auto& x = a.scenes[static_cast<std::size_t>(s)];
        const auto& y = b.scenes[static_cast<std::size_t>(s)];
        const std::string at = "scene " + std::to_string(s + 1);
        if (x.name != y.name || x.colour != y.colour || x.bpm != y.bpm)
            return at + " props";
        if (! (x.extras == y.extras))
            return at + " extras\n" + extrasText(x.extras) + "vs\n" + extrasText(y.extras);
        for (int t = 0; t < 16; ++t)
        {
            const auto& p = x.tracks[static_cast<std::size_t>(t)];
            const auto& q = y.tracks[static_cast<std::size_t>(t)];
            const std::string tt = at + " track " + std::to_string(t + 1);
            if (! (p.playback == q.playback))
                return tt + " playback\n" + extrasText(p.playback.extras) + "vs\n" + extrasText(q.playback.extras);
            if (! (p.ampEnv == q.ampEnv))
                return tt + " ampEnv";
            if (! (p.seq == q.seq))
                return tt + " seq\n" + extrasText(p.seq.extras) + "vs\n" + extrasText(q.seq.extras);
            if (! (p.slices == q.slices))
                return tt + " slices";
            if (! (p.padScale == q.padScale) || ! (p.midiScale == q.midiScale))
                return tt + " scales";
            for (int k = 0; k < 2; ++k)
                if (! (p.insertFx[static_cast<std::size_t>(k)] == q.insertFx[static_cast<std::size_t>(k)]))
                    return tt + " insert " + std::to_string(k + 1) + "\n" +
                           extrasText(p.insertFx[static_cast<std::size_t>(k)].extras) + "vs\n" +
                           extrasText(q.insertFx[static_cast<std::size_t>(k)].extras);
            if (! (p.send == q.send))
                return tt + " send";
            if (! (p.lfo == q.lfo))
                return tt + " lfo\n" + extrasText(p.lfo.extras) + "vs\n" + extrasText(q.lfo.extras);
            if (! (p.midi == q.midi))
                return tt + " midi\n" + extrasText(p.midi.extras) + "vs\n" + extrasText(q.midi.extras);
            if (! (p.routing == q.routing))
                return tt + " routing";
            if (! (p == q))
                return tt + " track\n" + extrasText(p.extras) + "vs\n" + extrasText(q.extras);
        }
        for (int k = 0; k < 16; ++k)
        {
            const auto& p = x.patterns[static_cast<std::size_t>(k)];
            const auto& q = y.patterns[static_cast<std::size_t>(k)];
            if (! (p == q))
                return at + " pattern " + std::to_string(k + 1) + " (len " + std::to_string(p.length) + "/" +
                       std::to_string(q.length) + ", locks " + std::to_string(p.locks.size()) + "/" +
                       std::to_string(q.locks.size()) + ")\n" + extrasText(p.extras) + "vs\n" + extrasText(q.extras);
        }
        if (! (x.sendFx == y.sendFx) || ! (x.masterFx == y.masterFx))
            return at + " bus fx\n" + extrasText(x.sendFx.extras) + extrasText(x.masterFx.extras) + "vs\n" +
                   extrasText(y.sendFx.extras) + extrasText(y.masterFx.extras);
        if (! (x.routing == y.routing))
            return at + " routing\n" + extrasText(x.routing.extras) + "vs\n" + extrasText(y.routing.extras);
    }
    if (! (a.arrangement == b.arrangement))
        return "arrangement\n" + extrasText(a.arrangement.extras) + "vs\n" + extrasText(b.arrangement.extras);
    if (! (a.ribbon == b.ribbon))
        return "ribbon\n" + extrasText(a.ribbon.extras) + "vs\n" + extrasText(b.ribbon.extras);
    if (! (a.midi == b.midi))
        return "midi";
    if (! (a.extras == b.extras))
        return "project extras\n" + extrasText(a.extras) + "vs\n" + extrasText(b.extras);
    if (a.sampleRefs.entries.size() != b.sampleRefs.entries.size())
        return "sample pool size";
    for (std::size_t k = 0; k < a.sampleRefs.entries.size(); ++k)
    {
        auto x = a.sampleRefs.entries[k], y = b.sampleRefs.entries[k];
        if (x.id != y.id || x.missing != y.missing || x.displayName != y.displayName ||
            juce::File(x.path).getFileName() != juce::File(y.path).getFileName() || ! (x.extras == y.extras))
            return "sample " + x.id + " (" + x.path + " vs " + y.path + ")";
    }
    return {};
}

void stripExtras(m::Project& p)
{
    p.extras = {};
    p.settings.extras = {};
    for (auto& s : p.scenes)
    {
        s.extras = {};
        s.routing.extras = {};
        s.sendFx.extras = {};
        s.masterFx.extras = {};
        for (auto& t : s.tracks)
        {
            t.extras = {};
            t.playback.extras = t.ampEnv.extras = t.seq.extras = t.slices.extras = {};
            t.padScale.extras = t.midiScale.extras = t.send.extras = t.lfo.extras = t.midi.extras = {};
            t.routing.extras = {};
            t.insertFx[0].extras = t.insertFx[1].extras = {};
        }
        for (auto& pat : s.patterns)
            pat.extras = {};
    }
    p.arrangement.extras = p.ribbon.extras = p.midi.extras = {};
}

ExportReport exportTo(const m::Project& p, const juce::File& folder, const std::string& name)
{
    ExportOptions o;
    o.name = name;
    ExportReport report;
    auto r = exportProject(p, folder, o, report);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    return report;
}

bool prjDumpRoundTrip(const juce::File& prj, juce::String& out)
{
    return interop_test::runTool({"python3",
                                  interop_test::sourceDir().getChildFile("tools/prj-dump/prj-dump.py").getFullPathName(),
                                  prj.getFullPathName(), "--roundtrip"},
                                 &out);
}

struct FlagsOn
{
    FlagsOn()
    {
        services::ext::flags().set(services::ext::kMaster, true);
        services::ext::flags().set(services::ext::kExport, true);
    }
    ~FlagsOn() { services::ext::flags().clearOverrides(); }
};

void writeSine(const juce::File& f, double rate, double seconds, int channels)
{
    f.deleteFile();
    services::render::AudioWriterSpec spec;
    spec.format = services::render::SampleFormat::Pcm24;
    spec.channels = channels;
    spec.sampleRate = rate;
    auto w = services::render::AudioFileWriter::create(f, spec);
    REQUIRE(w);
    const int n = static_cast<int>(rate * seconds);
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));
    const float* ch[2] = {x.data(), x.data()};
    REQUIRE(w.value()->writePlanar(ch, n));
    REQUIRE(w.value()->finish());
}
} // namespace

TEST_CASE("ext export: generated -> import -> export -> import is equal at model level (observed schema)",
          "[ext][interop][export]")
{
    INTEROP_FIXTURE(g, "observed");
    const auto a = importFile(g.prj);
    const auto dir = tempDir("observed");
    const auto report = exportTo(*a, dir, "Observed");
    for (const auto& l : report.lines())
        UNSCOPED_INFO("EXPORT " << l);
    REQUIRE(report.projectFile.existsAsFile());
    CHECK(report.samples == 2);
    CHECK(report.samplesCopied == 2); // already 44.1 kHz 16-bit WAV: byte copies
    CHECK(report.samplesMissing == 0);
    CHECK(dir.getChildFile("PIONEER DJ SAMPLER/Samples/Kit/kick.wav").existsAsFile());
    CHECK(dir.getChildFile("PIONEER DJ SAMPLER/Samples/Kit/kick.wav.dat").existsAsFile());
    CHECK(dir.getChildFile("PIONEER DJ SAMPLER/Samples/Kit/tone.wav.dat").existsAsFile());
    CHECK_FALSE(dir.getSiblingFile("observed.partial").exists());

    const auto b = importFile(report.projectFile);
    const auto diff = firstDifference(*a, *b);
    INFO(diff);
    CHECK(diff.empty());

    // The exported file decodes with the independent Python decoder and re-encodes byte for byte.
    juce::String out;
    const bool ok = prjDumpRoundTrip(report.projectFile, out);
    INFO(out.toStdString());
    CHECK(ok);
    CHECK(out.contains("identical"));

    // A second generation is byte-identical (the export is a function of the model).
    const auto dir2 = tempDir("observed2");
    const auto report2 = exportTo(*b, dir2, "Observed");
    CHECK(interop_test::bytesOf(report2.projectFile) == interop_test::bytesOf(report.projectFile));
}

TEST_CASE("ext export: the spec-schema fixtures round-trip their model fields", "[ext][interop][export]")
{
    for (const char* name : {"basic", "full"})
    {
        INTEROP_FIXTURE(g, name);
        auto a = importFile(g.prj);
        const auto dir = tempDir(std::string("spec-") + name);
        const auto report = exportTo(*a, dir, name);
        auto b = importFile(report.projectFile);
        // A second generation from the re-import is model-equal including what the import kept.
        const auto dir2 = tempDir(std::string("spec2-") + name);
        const auto report2 = exportTo(*b, dir2, name);
        auto c = importFile(report2.projectFile);
        {
            const auto diff = firstDifference(*b, *c);
            INFO(name << " (2nd generation): " << diff);
            CHECK(diff.empty());
        }
        // The spec schema's names and int encodings are written in the observed forms: the model fields are equal,
        // the kept originals may differ (e.g. a plain-int retriggerSpeed becomes a packed hi/lo kept by the import).
        stripExtras(*a);
        stripExtras(*b);
        // Arrangement rows the format cannot hold are not imported either; the pool ids stay "sp16:<url>".
        const auto diff = firstDifference(*a, *b);
        INFO(name << ": " << diff);
        CHECK(diff.empty());
    }
}

TEST_CASE("ext export: a native project exports, samples become 44.1 kHz WAV <= 64 s, dropped data is reported",
          "[ext][interop][export]")
{
    const auto src = tempDir("native-src");
    REQUIRE(src.createDirectory());
    const auto wav48 = src.getChildFile("loop48.wav");
    writeSine(wav48, 48000.0, 0.5, 2);
    const auto longWav = src.getChildFile("long.wav");
    writeSine(longWav, 44100.0, 65.0, 1);

    auto p = m::newDefaultProject();
    p->settings.tempo = 133.0;
    p->settings.swing = 60;
    p->settings.patternQuantize = m::QuantizeUnit::Bar;
    auto& tr = p->scenes[0].tracks[0];
    tr.volume = 90;
    tr.chokeGroup = 2;
    tr.name = "KICK";
    tr.playback.sample.id = "hash-a";
    tr.insertFx[0].type = m::EffectType::MultiModeFilter;
    tr.insertFx[0].type1 = 1;
    tr.insertFx[0].params[0] = 0.5;
    p->scenes[0].tracks[1].playback.sample.id = "hash-b";
    p->scenes[0].tracks[2].playback.sample.id = "hash-missing";
    p->sampleRefs.entries.push_back({"hash-a", wav48.getFullPathName().toStdString(), "loop48.wav", false, {}});
    p->sampleRefs.entries.push_back({"hash-b", longWav.getFullPathName().toStdString(), "long.wav", false, {}});
    p->sampleRefs.entries.push_back({"hash-missing", "/nonexistent/gone.wav", "gone.wav", true, {}});
    auto& pat = p->scenes[0].patterns[0];
    pat.length = 32;
    pat.steps[0][0].trigger = 100;
    pat.steps[0][0].value2 = 100;
    pat.steps[4][2].trigger = 11; // step 5 track 3, Half
    pat.steps[4][2].value2 = 90;
    pat.steps[4][2].triggerType = m::TriggerType::Half;
    pat.steps[31][15].trigger = 15; // step 32 track 16
    pat.steps[31][15].value2 = 127;
    const int pitchSlot = core::commands::sequencer::lockSlotOf(*core::builtinParams().find("playback.pitch"));
    REQUIRE(pitchSlot >= 0);
    m::StepLocks l;
    l.mask = std::uint64_t {1} << pitchSlot; // ordinal 0 (raw × 0.01) in the built-in map
    l.values[static_cast<std::size_t>(pitchSlot)] = -7.0f;
    l.change[static_cast<std::size_t>(pitchSlot)] = m::LockChange::Hold;
    pat.locks.push_back(l);
    pat.steps[0][0].lockIndex = 0;
    p->arrangement.sections.push_back({});
    p->scenes[0].tracks[3].extras.set("native.ext.trk.toggle", "true");

    const auto dir = tempDir("native");
    const auto report = exportTo(*p, dir, "Native");
    const auto lines = report.lines();
    std::string all;
    for (const auto& l2 : lines)
        all += l2 + "\n";
    INFO(all);
    CHECK(report.samples == 3);
    CHECK(report.samplesConverted == 2);
    CHECK(report.samplesTruncated == 1);
    CHECK(report.samplesMissing == 1);
    for (const char* what : {"track names", "arrangement rows", "HOLD / GLIDE", "beyond 64 s", "missing sample",
                             "extension data ext.trk.toggle"})
        CHECK(all.find(what) != std::string::npos);

    for (const char* name : {"loop48.wav", "long.wav"})
    {
        const auto f = dir.getChildFile("PIONEER DJ SAMPLER/Samples").getChildFile(name);
        auto info = services::library::probeAudioFile(f);
        REQUIRE(info);
        CHECK(info.value().sampleRate == 44100.0);
        CHECK(info.value().seconds() <= 64.0);
        CHECK(info.value().encoding == services::library::PcmEncoding::S24);
        CHECK(juce::File(f.getFullPathName() + ".dat").existsAsFile());
    }
    {
        auto info = services::library::probeAudioFile(dir.getChildFile("PIONEER DJ SAMPLER/Samples/loop48.wav"));
        REQUIRE(info);
        CHECK(std::llabs(info.value().frames - 22050) <= 1);
        CHECK(info.value().channels == 2);
    }

    const auto b = importFile(report.projectFile);
    CHECK(b->settings.tempo == 133.0);
    CHECK(b->settings.swing == 60);
    CHECK(b->settings.patternQuantize == m::QuantizeUnit::Bar);
    const auto& bt = b->scenes[0].tracks[0];
    CHECK(bt.volume == 90);
    CHECK(bt.chokeGroup == 2);
    CHECK(bt.insertFx[0].type == m::EffectType::MultiModeFilter);
    CHECK(bt.insertFx[0].type1 == 1);
    CHECK(bt.insertFx[0].params[0] == 0.5);
    const auto& bp = b->scenes[0].patterns[0];
    CHECK(bp.length == 32);
    CHECK(bp.steps[0][0].trigger == 15); // the model's Full code: the record holds coordinates, not a trigger value
    CHECK(bp.steps[4][2].trigger == 11);
    CHECK(bp.steps[4][2].triggerType == m::TriggerType::Half);
    CHECK(bp.steps[2][4].trigger == m::Step::kEmpty); // not transposed
    CHECK(bp.steps[31][15].trigger == 15);
    {
        // The written records: track-major, each programmed one naming its own track and step.
        auto tree = readContainer(interop_test::bytesOf(report.projectFile));
        REQUIRE(tree);
        const Node* patNode = nullptr;
        for (const auto& sc : tree.value().children)
            if (sc.type == "SceneData0")
                for (const auto& pn : sc.children)
                    if (pn.type == "PatternData0")
                        patNode = &pn;
        REQUIRE(patNode != nullptr);
        auto recs = decodeSteps(patNode->prop("Steps")->bin);
        REQUIRE(recs);
        int programmed = 0;
        for (std::size_t k = 0; k < recs.value().size(); ++k)
            if (recs.value()[k].programmed())
            {
                ++programmed;
                CHECK(recs.value()[k].namesPosition(stepOfRecord(k), trackOfRecord(k)));
            }
        CHECK(programmed == 3);
        CHECK(recs.value()[recordIndex(4, 2)].trigger == 2);
        CHECK(recs.value()[recordIndex(4, 2)].value2 == 4);
        CHECK(recs.value()[recordIndex(4, 2)].typeFlags == 11);
        CHECK(recs.value()[recordIndex(31, 15)].trigger == 15);
        CHECK(recs.value()[recordIndex(31, 15)].value2 == 31);
    }
    REQUIRE(bp.steps[0][0].lockIndex != m::Step::kNoLocks);
    CHECK(bp.locks[bp.steps[0][0].lockIndex].values[static_cast<std::size_t>(pitchSlot)] == -7.0f);
    CHECK(b->sampleRefs.entries.size() == 3);
    int found = 0;
    for (const auto& e : b->sampleRefs.entries)
        found += e.missing ? 0 : 1;
    CHECK(found == 2);
    juce::String out;
    CHECK(prjDumpRoundTrip(report.projectFile, out));
}

TEST_CASE("ext export: SET SCALE's 22 scales round-trip by name; the importer reads the manual's names loosely",
          "[ext][interop][export]")
{
    auto p = m::newDefaultProject();
    const int n = static_cast<int>(m::enumCount<m::ScaleType>());
    REQUIRE(n == 22);
    for (int k = 0; k < n; ++k) // every scale on a pad scale and a MIDI scale
    {
        auto& tr = p->scenes[static_cast<std::size_t>(k / 16)].tracks[static_cast<std::size_t>(k % 16)];
        tr.padScale.scaleType = k;
        tr.midiScale.scaleTypeMidi = n - 1 - k;
    }
    const auto dir = tempDir("scales");
    const auto report = exportTo(*p, dir, "Scales");
    const auto b = importFile(report.projectFile);
    for (int k = 0; k < n; ++k)
    {
        INFO("scale " << k);
        const auto& tr = b->scenes[static_cast<std::size_t>(k / 16)].tracks[static_cast<std::size_t>(k % 16)];
        CHECK(tr.padScale.scaleType == k);
        CHECK(tr.midiScale.scaleTypeMidi == n - 1 - k);
    }

    // The exported ScaleSetting names; a file naming the scales in other spellings of the manual's names (or by the
    // list index, No. − 1) reads the same scales.
    auto tree = readContainer(interop_test::bytesOf(report.projectFile));
    REQUIRE(tree);
    std::vector<Node*> pads, midis;
    std::function<void(Node&)> walk = [&](Node& node)
    {
        if (node.type == "ScaleSetting")
            pads.push_back(&node);
        else if (node.type == "ScaleSettingMidi")
            midis.push_back(&node);
        for (auto& c : node.children)
            walk(c);
    };
    walk(tree.value());
    REQUIRE(pads.size() >= 3);
    REQUIRE(midis.size() >= 2);
    auto set = [](Node& node, std::string_view name, Var v)
    {
        for (auto& [k, value] : node.props)
            if (k == name)
                value = std::move(v);
    };
    REQUIRE(pads[1]->prop("scaleType") != nullptr);
    CHECK(pads[1]->prop("scaleType")->s == "Ionian");
    set(*pads[0], "scaleType", Var::ofString("MAJOR_PENTATONIC"));
    set(*pads[1], "scaleType", Var::ofString("Scale_Raga Todi"));
    set(*pads[2], "scaleType", Var::ofInt(19)); // No. 20 Hawaiian
    set(*midis[0], "scaleTypeMidi", Var::ofString("combination diminish"));
    set(*midis[1], "scaleTypeMidi", Var::ofString("HarmonicMinor")); // not a scale of the manual: kept, reported
    const auto edited = dir.getChildFile("edited.prj");
    const auto bytes = writeContainer(tree.value());
    REQUIRE(edited.replaceWithData(bytes.data(), bytes.size()));
    const auto c = importFile(edited);
    const auto& t = c->scenes[0].tracks;
    CHECK(t[0].padScale.scaleType == static_cast<int>(m::ScaleType::MajorPentatonic));
    CHECK(t[1].padScale.scaleType == static_cast<int>(m::ScaleType::RagaTodi));
    CHECK(t[2].padScale.scaleType == static_cast<int>(m::ScaleType::Hawaiian));
    CHECK(t[0].midiScale.scaleTypeMidi == static_cast<int>(m::ScaleType::CombinationDiminish));
    CHECK(t[1].midiScale.scaleTypeMidi == 0); // unmapped: the default stays, the text is kept
    CHECK(t[1].midiScale.extras.find("sp16.ScaleSettingMidi.scaleTypeMidi") != nullptr);
}

TEST_CASE("ext export: always a new folder, never an instrument disk, never an overwrite", "[ext][interop][export]")
{
    auto p = m::newDefaultProject();
    const auto base = tempDir("safety");
    REQUIRE(base.createDirectory());
    ExportReport report;
    // An existing folder is refused, its content untouched.
    const auto existing = base.getChildFile("exists");
    REQUIRE(existing.createDirectory());
    REQUIRE(existing.getChildFile("keep.txt").replaceWithText("mine"));
    CHECK_FALSE(exportProject(*p, existing, {}, report));
    CHECK(existing.getChildFile("keep.txt").loadFileAsString() == "mine");
    // An instrument disk layout (or a folder below one) is refused.
    const auto disk = base.getChildFile("STICK");
    REQUIRE(disk.getChildFile("PIONEER DJ SAMPLER/System").createDirectory());
    CHECK(isInstrumentDisk(disk.getChildFile("new")));
    CHECK_FALSE(exportProject(*p, disk.getChildFile("new"), {}, report));
    CHECK_FALSE(disk.getChildFile("new").exists());
    CHECK(isInstrumentDisk(disk.getChildFile("PIONEER DJ SAMPLER/Projects/x")));
    CHECK(isInstrumentDisk(base.getChildFile("TORAIZ/sub")));
    CHECK_FALSE(isInstrumentDisk(base.getChildFile("fresh")));
    // A default project exports and decodes.
    CHECK(exportProject(*p, base.getChildFile("fresh"), {}, report));
    CHECK(base.getChildFile("fresh/PIONEER DJ SAMPLER/Projects/fresh.prj").existsAsFile());
    CHECK_FALSE(base.getChildFile("fresh/PIONEER DJ SAMPLER/System").exists()); // no resume file: the unit's state
}

TEST_CASE("ext export: the codec writes only with ext.enabled + ext.interop.export, never reads",
          "[ext][interop][export]")
{
    const auto* c = store::findCodec("sp16-prj-export");
    REQUIRE(c != nullptr);
    CHECK(c->info().canWrite);
    CHECK(store::codecForExtension(".prj") == store::findCodec("sp16-prj")); // the importer still opens .prj
    INTEROP_FIXTURE(g, "basic");
    CHECK_FALSE(c->canRead(g.prj));
    CHECK(store::codecFor(g.prj) == store::findCodec("sp16-prj"));
    store::ImportReport ir;
    CHECK_FALSE(c->read(g.prj, ir));

    auto p = m::newDefaultProject();
    const auto base = tempDir("codec");
    REQUIRE(base.createDirectory());
    services::ext::flags().clearOverrides();
    CHECK_FALSE(services::ext::flags().isOn(services::ext::kExport));
    CHECK_FALSE(c->write(*p, base.getChildFile("off")));
    CHECK_FALSE(base.getChildFile("off").exists());
    {
        services::ext::flags().set(services::ext::kExport, true); // without the master switch: still off
        CHECK_FALSE(c->write(*p, base.getChildFile("off2")));
        services::ext::flags().clearOverrides();
    }
    FlagsOn on;
    CHECK(c->write(*p, base.getChildFile("on")));
    CHECK(base.getChildFile("on/PIONEER DJ SAMPLER/Projects/on.prj").existsAsFile());
}
