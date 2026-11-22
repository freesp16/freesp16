// Read access to the SCENE MANAGER body for tests and benchmarks (tests/snapshot/m2, tests/bench/bench_ui_m2.cpp).
#pragma once

#include "ui/widgets/SceneKey.h"

namespace ui
{
struct SceneManagerView
{
    int tab = 0;   // 0 PATTERN, 1 SEQ., 2 TRACK
    int level = 0; // PATTERN tab: 0 = scene row, 1 = pattern row
    int focusScene = 1, focusPattern = 1, chosenTrack = 0;
    bool copyEnabled = false, pasteEnabled = false, deleteEnabled = false;
    int pendingScene = 0, pendingPattern = 0; // 1-based, 0 = none
    int seqStartBar = 0;                      // SEQ. selection start, 0-based
};
// The body's state now (false when `body` is not the SCENE MANAGER).
bool sceneManagerView(Widget* body, SceneManagerView& out);
// The model of PATTERN-tab key n (1–16) of the scene row (`patternRow` false) or the pattern row, at `nowMs`.
SceneKeyModel sceneManagerKey(Widget* body, bool patternRow, int n, double nowMs);
// Whether a commit while playing is queued for the engine (scene.queue) instead of applied at once. Default: only when
// a real `scene.queue` ParamSpec is registered; tests force it.
void setSceneQueueHandledByEngine(int forced); // 1 = yes, 0 = no, -1 = automatic
// Tests: forget the remembered tab, the clipboard and INCLUDE SEQUENCE.
void resetSceneManager();
} // namespace ui
