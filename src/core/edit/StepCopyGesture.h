// The panel's step copy gesture (spec editing-and-undo.md EDT-005/006, control-surface.md UI-102 extension
// controls `ext.copy` / `ext.paste`): hold COPY + press step keys → those steps (of the selected track, or of all
// tracks with `allTracks`) are copied when COPY is released; hold PASTE + press a step key → the clipboard is pasted
// from that step (wrapping at the pattern length). COPY held with no step pressed copies the shown page (bar,
// our choice): "copy whole bar" (EDT table "Page / bar"). The step controller (ui/input/steps) feeds it the keys while
// COPY / PASTE are held and leaves the steps alone then. Message thread only; the edits go through the SceneEditor.
#pragma once

#include "core/edit/SceneEditor.h"

#include <cstdint>

namespace core::edit
{
class StepCopyGesture
{
public:
    explicit StepCopyGesture(SceneEditor& editor) noexcept : editor_(&editor) {}
    void setEditor(SceneEditor& editor) noexcept { editor_ = &editor; } // the shared editor may be rebound

    // Where the steps are: scene, pattern, the selected track (kAllTracks = all 16) and the page the keys show (0–3).
    void setTarget(int scene, int pattern, int track, int shownPage) noexcept;

    void copyPressed() noexcept;
    void copyReleased(); // copies the pressed steps, or the shown page when none was pressed
    void pastePressed() noexcept;
    void pasteReleased() noexcept;
    // A step key (1–16 on the shown page) while COPY or PASTE is held; false (not consumed) otherwise.
    bool stepKey(int key);

    bool active() const noexcept { return copyHeld_ || pasteHeld_; }
    std::uint64_t pending() const noexcept { return mask_; } // steps pressed so far while COPY is held

private:
    SceneEditor* editor_;
    int scene_ = 0, pattern_ = 0, track_ = 0, page_ = 0;
    bool copyHeld_ = false, pasteHeld_ = false;
    std::uint64_t mask_ = 0;
};
} // namespace core::edit
