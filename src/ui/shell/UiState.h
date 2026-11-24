// The shell's view state: what the frame shows and every screen reads (selected track UI-205/UID-502, transport
// indicators, context-bar readouts, SHIFT and step-hold state UID-161/166, master meter, the track summaries HOME
// draws). The shell owns it, fills it from the engine feeds once per frame, and hands a pointer to every body widget
// (WidgetArgs::state). Strings are fixed buffers or views of storage that outlives the frame: reading it never
// allocates.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace ui
{
struct TrackSummary
{
    std::string_view name{};                     // sample / source name; empty = empty track
    std::string_view typeBadge = "SMPL";         // SMPL / THRU / MIDI / AS-1
    std::string_view stretchBadge = "OFF";       // OFF / RSMPL / M.TMP
    std::string_view loopGlyph = "\xE2\x86\x92"; // → one shot (home.md "Tile status glyphs")
    bool hasContent = false;
    bool muted = false, soloed = false, missing = false;
    bool unsavedTake = false; // plays a recorded take not saved yet: the tile's UNSAVED TAKE badge (sampling.md W3)
    double lastHitMs = -1e9; // trigger flash start (pad hit or meter feed)
    // SET SCALE's selected key (the manual's 1.6: the key last selected by pressing a pad, drawn white): the pitch that
    // pad played, as semitones over the track's PITCH (SAMPLE) or the MIDI note (MIDI / AS-1); kNoKey = none yet.
    static constexpr int kNoKey = -10000;
    int scaleKey = kNoKey;
    float levelDb = -120.0f; // mini level
};

// A short fixed text (BPM "120.0", position "001:01:01"): set with printf-style formatting, read as a view.
template <std::size_t N> struct FixedText
{
    char text[N]{};
    int length = 0;
    FixedText() = default;
    FixedText(std::string_view s) { set(s); } // NOLINT: implicit from literals in fixtures
    void set(std::string_view s) noexcept
    {
        length = static_cast<int>(s.size() < N - 1 ? s.size() : N - 1);
        for (int i = 0; i < length; ++i)
            text[i] = s[static_cast<std::size_t>(i)];
        text[length] = 0;
    }
    template <typename... A> void format(const char* fmt, A... args) noexcept
    {
        const int n = std::snprintf(text, N, fmt, args...);
        length = n < 0 ? 0 : (n < static_cast<int>(N) ? n : static_cast<int>(N) - 1);
    }
    std::string_view view() const noexcept { return {text, static_cast<std::size_t>(length)}; }
};

struct UiState
{
    int selectedTrack = 1; // 1–16, persists across screens (UID-502)
    int scene = 1, pattern = 1;
    bool playing = false, recArmed = false, dirty = false;
    bool playArmed = false; // MIDI clock sync: PLAY pressed, waiting for the external Start (PLAY lamp / ▶ blink)
    bool shift = false; // SHIFT held: fine steps + FINE captions (UID-161)
    int stepHeld = 0;   // 1–16 while a step key is held (UID-166), 0 = none
    // BYPASS mode (UI-221): lit by the context bar's BYPASS; while on, a tap or push on a bypassable module toggles
    // its bypass instead of focusing or opening it. Ends with a second BYPASS tap or when the screen changes.
    bool bypassMode = false;
    FixedText<12> bpm{"120.0"}, quantize{"1/16"}, position{"001:01:01"};
    std::string_view syncBadge{}; // empty = INTERNAL (hidden); "MIDI" / "LINK"
    bool syncLost = false;
    double midiInAtMs = -1e9, midiOutAtMs = -1e9;
    float masterDb[2] = {-120.0f, -120.0f}, masterPeakDb[2] = {-120.0f, -120.0f};
    double masterPeakAtMs[2] = {0, 0};
    float masterGain = 1.0f;
    std::array<TrackSummary, 16> tracks{};
    bool dark = true;          // UID-051 (SET preference), default dark
    bool reduceMotion = false; // UID-066
    // The SCENE | PATTERN context item is greyed (UI-206): set by the body each frame while switching is
    // blocked (SAMPLING/EDIT open, SMP-008; the song plays, ARR-007); the shell clears it on every screen entry.
    bool sceneLocked = false;
    // The sampler records / waits (SAMPLING/EDIT, LIVE SAMPLING: UID-777) — the status-row REC shows it too;
    // set by the body each frame (blinking in standby), cleared on every screen entry.
    bool samplerRec = false;
    // The pads' MUTE key, for the status row's MUTE / SOLO badge: Mute = MUTE pad mode (pads toggle mutes), Solo = the
    // MUTE key held (pads toggle solos). Set by the composition root every frame from the pad controller.
    enum class PadMuteView : std::uint8_t { None, Mute, Solo } padMuteView = PadMuteView::None;

    const TrackSummary& selected() const noexcept { return tracks[static_cast<std::size_t>((selectedTrack - 1) & 15)]; }
    // engine::ParamChange scope of a track parameter of the selected track in the current scene.
    std::uint16_t trackScope(int track1Based = 0) const noexcept
    {
        const int t = track1Based > 0 ? track1Based : selectedTrack;
        return static_cast<std::uint16_t>(((scene - 1) & 15) << 4 | ((t - 1) & 15));
    }
};
} // namespace ui
