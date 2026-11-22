// MASTER TEMPO time-stretching (PLY-030…034). Backends register with APP_REGISTER_STRETCHER in
// engine/voice/stretch/<Backend>.cpp and are chosen by the global setting audio.stretchBackend (SET); the built-in
// backends are in engine/voice/stretch/ (pvoc, wsola, none). process() is real-time (noexcept, no allocation);
// prepare() allocates on the message thread.
#pragma once

#include "core/util/Registrar.h"

#include <memory>
#include <string_view>
#include <vector>

namespace engine::voice
{
class TimeStretcher {                                 // MASTER TEMPO mode (PLY-030…034)
public:
    virtual ~TimeStretcher() = default;
    virtual void prepare(double sampleRate, int maxBlock, int channels) = 0;   // allocates
    virtual void reset() noexcept = 0;
    // timeRatio: output duration / input duration (2 = twice as long); pitchScale: frequency factor (1 = unchanged).
    virtual void setRatios(double timeRatio, double pitchScale) noexcept = 0;
    virtual int latencySamples() const noexcept = 0;
    // Consumes inFrames (== framesNeeded(outFrames)) planar input frames, writes outFrames; returns frames written.
    virtual int process(const float* const* in, int inFrames, float* const* out, int outFrames) noexcept = 0;
    virtual int framesNeeded(int outFrames) const noexcept = 0;
};
using StretcherFactory = std::unique_ptr<TimeStretcher> (*)();

class StretcherRegistry
{
public:
    struct Entry
    {
        std::string_view name; // "signalsmith", "rubberband", "none"
        StretcherFactory create;
    };
    static StretcherRegistry& builtin() noexcept
    {
        static StretcherRegistry registry;
        return registry;
    }
    bool add(std::string_view name, StretcherFactory create)
    {
        if (name.empty() || create == nullptr || find(name) != nullptr)
            return false;
        entries_.push_back({name, create}); // rt-ok: start-up registration, main thread
        return true;
    }
    StretcherFactory find(std::string_view name) const noexcept
    {
        for (const auto& e : entries_)
            if (e.name == name)
                return e.create;
        return nullptr;
    }
    const std::vector<Entry>& all() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};
} // namespace engine::voice

// APP_REGISTER_STRETCHER(name, factory): static registrar into StretcherRegistry::builtin().
#define APP_REGISTER_STRETCHER(name, factory)                                                                          \
    APP_STATIC_REGISTRAR(::engine::voice::StretcherRegistry::builtin().add(name, factory))
