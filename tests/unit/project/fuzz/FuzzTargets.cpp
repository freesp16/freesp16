#include "FuzzTargets.h"

#include "core/model/ModelOps.h"
#include "services/library/AudioFile.h"
#include "services/library/Decoder.h"
#include "services/library/SidecarStore.h"
#include "services/store/Autosave.h"
#include "services/store/Migrations.h"
#include "services/store/NativeCodec.h"
#include "services/store/ProjectStore.h"

#include <juce_core/juce_core.h>

#include <array>
#include <memory>
#include <mutex>

namespace projfuzz
{
namespace m = core::model;
namespace st = services::store;

namespace
{
struct Defaults
{
    std::string project, meta;
    std::array<std::string, m::kScenes> scenes;
};

const Defaults& defaults()
{
    static Defaults d;
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       st::SaveOptions o;
                       o.timestamp = "2026-01-01T00:00:00.000Z";
                       const auto p = m::newDefaultProject();
                       for (auto& f : st::NativeCodec::encode(*p, o))
                       {
                           if (f.name == "project.json")
                               d.project = f.text;
                           else if (f.name == "meta.json")
                               d.meta = f.text;
                           else if (f.name.rfind("patterns/scene-", 0) == 0)
                               d.scenes[static_cast<std::size_t>(std::atoi(f.name.c_str() + 15) - 1) % 16] = f.text;
                       }
                   });
    return d;
}

// The store's guard runs before every parse (ProjectStore checkBundleInputs, Autosave::decodeText).
juce::var parse(const std::string& text)
{
    if (! st::jsonNestingWithin(text))
        return {};
    juce::var v;
    if (juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), v).failed())
        return {};
    return v;
}

// The project lives on the heap (≈ 2.4 MB): never on a fuzzer's stack.
m::Project& scratch()
{
    static auto p = std::make_unique<m::Project>();
    return *p;
}

void decodeRoot(juce::var root)
{
    if (! root["project"].isObject())
        return; // readInto refuses it ("damaged")
    if (root["meta"]["format"]["name"].toString().toStdString() != st::kNativeFormatName)
        return;
    const int version = st::schemaVersionOf(root);
    const auto& runner = st::MigrationRunner::builtin();
    if (version <= runner.currentVersion() && ! runner.run(root, version))
        return;
    st::ImportReport report;
    auto& p = scratch();
    st::NativeCodec::decode(root, p, report);
    std::vector<std::string> problems;
    (void) m::validate(p, problems, 20);
    (void) st::NativeCodec::encode(p); // whatever decoded must encode again
}
} // namespace

const std::string& defaultFile(BundleFile which)
{
    const auto& d = defaults();
    return which == BundleFile::Project ? d.project : which == BundleFile::Meta ? d.meta : d.scenes[0];
}

int bundleFile(BundleFile which, const std::uint8_t* data, std::size_t size)
{
    const auto& d = defaults();
    const std::string text(reinterpret_cast<const char*>(data), size);
    auto* root = new juce::DynamicObject();
    juce::var rootVar(root);
    root->setProperty("project", parse(which == BundleFile::Project ? text : d.project));
    root->setProperty("meta", parse(which == BundleFile::Meta ? text : d.meta));
    juce::Array<juce::var> patterns;
    for (int s = 0; s < m::kScenes; ++s)
        patterns.add(parse(which == BundleFile::Scene && s == 0 ? text : d.scenes[static_cast<std::size_t>(s)]));
    root->setProperty("patterns", patterns);
    decodeRoot(rootVar);
    return 0;
}

int projectJson(const std::uint8_t* data, std::size_t size) { return bundleFile(BundleFile::Project, data, size); }

int autosave(const std::uint8_t* data, std::size_t size)
{
    st::ImportReport report;
    if (st::Autosave::decodeText(juce::String::fromUTF8(reinterpret_cast<const char*>(data), static_cast<int>(size)),
                                 scratch(), report))
        (void) st::NativeCodec::encode(scratch());
    return 0;
}

int audio(const std::uint8_t* data, std::size_t size)
{
    juce::MemoryInputStream in(data, size, false);
    (void) services::library::probeAudio(in);
    in.setPosition(0);
    services::library::DecodeReport report;
    services::library::DecodeOptions options;
    options.limitBytes = 64 << 20;
    (void) services::library::Decoder::decode(in, 44100.0, report, options);
    return 0;
}

int sidecar(const std::uint8_t* data, std::size_t size)
{
    (void) services::library::SidecarStore::parseReference(data, size);
    (void) services::library::SidecarStore::parseJson(
        juce::String::fromUTF8(reinterpret_cast<const char*>(data), static_cast<int>(size)));
    return 0;
}
} // namespace projfuzz
