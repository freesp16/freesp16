// What the screens know about a track's loaded sample: its overview pyramid (waveform widgets), length, rate and
// name. The ui-side view of the library's SamplePool::overview(Handle) + the model's SampleRef: the composition
// root installs the provider (app/LibraryAdapters.cpp, SampleViews::instance().setProvider()), tests their own; with
// no provider every track reads as empty (editor and thumbnails drawn empty, PLAYBACK columns 2–4 still editable).
#pragma once

#include "ui/widgets/WaveformEditor.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace ui::m1
{
struct SampleView
{
    const WaveOverview* overview = nullptr;
    std::int64_t frames = 0;
    double sampleRate = 44100;
    std::string_view name{};
    bool empty() const noexcept { return overview == nullptr || frames <= 0; }
};

class SampleViews
{
public:
    static SampleViews& instance() noexcept
    {
        static SampleViews views;
        return views;
    }
    void setProvider(std::function<SampleView(int track1Based)> p) { provider_ = std::move(p); }
    SampleView of(int track1Based) const { return provider_ ? provider_(track1Based) : SampleView{}; }

private:
    std::function<SampleView(int)> provider_;
};
} // namespace ui::m1
