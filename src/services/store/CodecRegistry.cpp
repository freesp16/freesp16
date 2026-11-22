#include "services/store/CodecRegistry.h"

#include "services/store/AtomicWriter.h"

#include <algorithm>
#include <memory>

namespace services::store
{
std::vector<const ProjectCodec*> codecs()
{
    std::vector<const ProjectCodec*> list;
    for (const auto& c : registeredCodecs())
        list.push_back(c.get());
    std::stable_sort(list.begin(), list.end(),
                     [](const ProjectCodec* a, const ProjectCodec* b)
                     {
                         const auto ia = a->info().id, ib = b->info().id;
                         if ((ia == "native") != (ib == "native"))
                             return ia == "native";
                         return ia < ib;
                     });
    return list;
}

const ProjectCodec* findCodec(std::string_view id)
{
    for (const auto* c : codecs())
        if (c->info().id == id)
            return c;
    return nullptr;
}

const ProjectCodec* codecForExtension(std::string_view ext)
{
    const auto wanted = juce::String::fromUTF8(ext.data(), static_cast<int>(ext.size()));
    for (const auto* c : codecs())
        for (const auto e : c->info().extensions)
            if (wanted.equalsIgnoreCase(juce::String::fromUTF8(e.data(), static_cast<int>(e.size()))))
                return c;
    return nullptr;
}

const ProjectCodec* codecFor(const juce::File& file)
{
    for (const auto* c : codecs())
        if (c->canRead(file))
            return c;
    return nullptr;
}

Result<void> openProjectInto(const juce::File& path, model::Project& out, ImportReport& report)
{
    const auto bundle = nativeBundleOf(path);
    if (bundle.getFileName().endsWithIgnoreCase(juce::String(kNativeExtension.data())))
        AtomicWriter::recover(bundle); // finishes or undoes an interrupted save (cheap when there is nothing to do)
    const auto* codec = codecFor(path);
    if (codec == nullptr)
        return Result<void>::failure("No project found at " + path.getFullPathName().toStdString());
    if (codec->info().id == "native")
        return NativeCodec::readInto(path, out, report);
    auto r = codec->read(path, report);
    if (! r)
        return r.error();
    out = std::move(*r.value());
    return {};
}

Result<std::unique_ptr<model::Project>> openProject(const juce::File& path, ImportReport& report)
{
    auto p = std::make_unique<model::Project>();
    if (auto r = openProjectInto(path, *p, report); ! r)
        return r.error();
    return p;
}

Result<std::unique_ptr<model::Project>> openProject(const juce::File& path)
{
    ImportReport report;
    return openProject(path, report);
}

Result<void> saveProject(const model::Project& p, const juce::File& bundle, const SaveOptions& options,
                         SaveReport* report)
{
    return NativeCodec::save(p, bundle, options, report);
}
} // namespace services::store
