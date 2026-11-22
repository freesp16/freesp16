#!/usr/bin/env python3
"""The live set check: "a 30-minute live set without the mouse … zero xruns, zero RT violations, LoadMeter p95
<= 50 % of the block, and every action reached by panel or keyboard (NFR-040)". Run by scripts/smoke/95-liveset.sh
(CHECK=liveset). Three subcommands:

  tools/liveset-check.py script --out FILE --manifest JSON [--minutes M] [--seed S]
      writes one long scripted-input session (ScriptedInput format, io/controllers/ScriptedInput.h) with the generated
      kit on scenes 1-2 (dev_kit_scenes=2). Every musical and navigation action is a panel control or a UI-120 keyboard
      key (pads, step keys, BAR keys, pad-mode keys, SCENE / PATTERN, SHIFT combinations, the six knobs, the selector and
      its push, BACK / HOME, PLAY / STOP / REC, the touch strip and its MODE / HOLD keys, ext.copy / ext.paste /
      ext.tap / ext.erase = Cmd+C / Cmd+V / N / J, and the jump keys ext.mixer / ext.browse /
      ext.utility / ext.project, ext.undo / ext.redo / ext.clear, ext.nudgeDown / ext.nudgeUp, SHIFT + SCENE, SHIFT +
      lit SCALE, SHIFT + ext.tap, SHIFT + sel.push for a screen's rail). `cmd` appears only for checkpoints (`state`,
      `ribbon`, `prefs`): every former NFR-040 gap (`save-as`, `open sceneManager`, `open track.playback.setScale`) is now
      played on the panel / keyboard (PROJECT > SAVE, SHIFT + SCENE, PLAYBACK's rail and SHIFT + lit SCALE).
      The first pass ("cycle 1", about 100 s) runs every routine once in a fixed order, so any run of 2 minutes or more
      covers every feature; the rest of the session picks routines at random (Python's random.Random(seed): the
      same seed gives the same script; the seed is in the script header, the manifest and the check's output). The set
      ends with STOP, STOP (the hard stop: tails silenced), 4 s of silence, checkpoints and quit.
      The manifest (JSON) lists the routines played, the features each one touched, the screens it navigated and the
      session's timing.
      The `settings` routine walks UTILITY and every sub-page (CHANNEL MAPPING IN / OUT, MIDI PORTS,
      AUDIO DEVICE, OUTPUT CHANNEL MAP, LIBRARY LOCATIONS; CONTROLLERS / KEYBOARD MAP when registered)
      with the rotary, push and BACK only, edits a project setting and a preference and puts both back (utility_rows()
      lists the rows the rotary visits).

  tools/liveset-check.py verify --log LOG --manifest JSON --script FILE [--wav WAV] [--exit-code N]
      one line per check, exit 1 when one fails:
        - exit code 0 and no FATAL / REALTIME VIOLATION in the log (host-fast and host have APP_RT_CHECKS: an
          allocation on the audio thread aborts the app with a "REALTIME VIOLATION" report, exit code 134)
        - the script parsed (SCRIPT ok) and only allowed `cmd` lines are in it (checkpoints + the marked GAP lines)
        - every AUDIO and LOAD line: xruns=0; LOAD lines present for the whole session (one per 10 s)
        - LOAD p95 <= 725 000 cycles/block in every LOAD line (each covers the last 10-20 s): 50 % of the 64-frame block
          at 44.1 kHz (1.451 ms) on the Cortex-A9's 1 GHz. On the desktop the LoadMeter counts 1 cycle = 1 ns, so this
          is a relative gate (a desktop core is several times faster than the A9; the real budget is confirmed on the
          hardware). overruns (blocks slower than real time) are reported, not gated
        - every scripted screen reached: one NAV <id> line per screen in the manifest
        - every save (PROJECT > SAVE on the panel): one SAVED <name> ok each, no SAVED … FAILED
        - lamps (lamp_log=1): each pad-mode key lit with its pad lamps (TRACK selection, dimmed mutes, SCALE roots,
          SLICE pads), the mode keys one-hot, no flicker in MUTE mode, every mode key off at the end
        - VmRSS growth bounded: max VmRSS of the last third of the session minus VmRSS at the first LOAD line after the
          kit is loaded (>= 20 s) <= 24 MB + 4 MB per 10 minutes. Why: the session's model grows only by the undo history
          (small commands: knob edits coalesce, step toggles, locks, short step pastes; the UndoStack cap is 256 MB, far
          above), the kit is loaded before the baseline, and the UI's per-screen allocations are freed on leaving; a leak
          of one allocation per event or per audio block (>= 10^4 events, 10^6 blocks per 30 min) would exceed it
        - no stuck notes: MAIN (WAV channels 0/1) peak <= -70 dBFS over the last 3 s (after the final STOP, STOP), and the
          set was not silent (peak > -30 dBFS somewhere before it)
        - coverage: each feature of the manifest has its evidence in the log (the action-coverage list is printed)

  tools/liveset-check.py coverage
      the NFR-040 audit: every performance action (and the live-set actions it relies on) with the panel / keyboard path that
      reaches it, from reading the app's input routing (app/Document.cpp, ui/shell/Shell.cpp, the performance screens,
      platform/host/input/KeyMap.cpp); a row with status GAP is reachable only by touch / mouse (or not at all). Exit 0.
Standard library only."""
import argparse
import json
import math
import pathlib
import random
import re
import struct
import sys
import wave

TAG = "[sp16-mod] "
LOAD_P95_MAX = 725000          # cycles/block: 50 % of 64 / 44100 s at 1 GHz (725.6 us)
RSS_BASE_MB, RSS_PER_10MIN_MB = 24.0, 4.0
SAVE_SPACING_MS = 15000        # a save starts >= 15 s after the previous one (the store refuses overlapping jobs)
SAVE_BUSY = "Another save or load is running"
TAIL_S, TAIL_MAX_DBFS, ACTIVE_MIN_DBFS = 3.0, -70.0, -30.0
ALLOWED_CMDS = ("state", "ribbon", "prefs")  # checkpoints
GAP_CMDS = ()                                # every NFR-040 gap has a panel / keyboard path now

# --- the UI-120 keyboard map (platform/host/input/KeyMap.cpp) and which controls the hardware panel has -----------------
KEYS = {}
for i, k in enumerate("ZXCV"):
    KEYS[f"pad.{1 + i}"] = k
for i, k in enumerate("ASDF"):
    KEYS[f"pad.{5 + i}"] = k
for i, k in enumerate("QWER"):
    KEYS[f"pad.{9 + i}"] = k
for i, k in enumerate("1234"):
    KEYS[f"pad.{13 + i}"] = k
for i, k in enumerate(["5", "6", "7", "8", "9", "0", "-", "=", "T", "Y", "U", "I", "O", "P", "[", "]"]):
    KEYS[f"step.{1 + i}"] = k
for i in range(4):
    KEYS[f"bar.{1 + i}"] = f"F{1 + i}"
KEYS.update({"padmode.track": "F5", "padmode.mute": "F6", "padmode.slice": "F7", "padmode.scale": "F8",
             "scene": "F9", "pattern": "F10", "strip.mode": "F11", "strip.hold": "F12", "play": "Space", "stop": "B",
             "rec": "G", "shift": "Shift", "back": "Esc", "home": "Home / `", "sel": ", / .", "sel.push": "Return",
             "ext.copy": "Cmd+C", "ext.paste": "Cmd+V", "ext.undo": "Cmd+Z", "ext.redo": "Shift+Cmd+Z",
             "ext.clear": "Delete", "ext.tap": "N", "ext.cue": "M", "ext.erase": "J", "ext.accent": "K",
             "ext.mixer": "Cmd+1", "ext.browse": "Cmd+2", "ext.utility": "Cmd+3", "ext.project": "Cmd+4",
             "ext.nudgeDown": "; (held, our choice)", "ext.nudgeUp": "' (held, our choice)"})
for n in range(1, 7):
    KEYS[f"enc.{n}"] = f"Ctrl+{n} then Up/Down (Left/Right: column)"


def panel_has(control: str) -> bool:
    return not control.startswith("ext.")


def canonical(name: str) -> str:
    for short, spec in (("pad", "pad."), ("step", "step."), ("bar", "bar."), ("knob", "enc.")):
        if name.startswith(short) and name[len(short):].isdigit():
            return spec + name[len(short):]
    return name


# --- UTILITY's focusable rows (ui/screens/utility/Utility.cpp) ------------------------------------------------------
ROOT = pathlib.Path(__file__).resolve().parent.parent


# The script runs on the reference hardware (hw-test bundle): no KEYBOARD MAP row there (no keyboard, UtilityEnv).
DEVICE = False


def utility_rows():
    """The rows the rotary visits on UTILITY, top to bottom (headers and disabled rows are skipped). Two depend on
    what is registered: SYNC SOURCE SELECT is enabled once the MIDI catalogue registers midi.syncSource, and
    CONTROLLERS / KEYBOARD MAP once their screens are registered (a row whose page is missing is disabled)."""
    src = ROOT / "src"
    midi = (src / "core/params/catalogue/midi.cpp").is_file()
    ctrl = any('"utility.controllers"' in f.read_text(errors="replace")
               for f in src.rglob("*.cpp") if "screens/utility/" not in str(f))
    rows = ["touchStrip.user1", "touchStrip.user2", "pitchRange", "patternQuantize", "padStartsSequence",
            "in.clock", "in.startStop", "in.noteCc", "in.channelMap", "in.channelMapReset", "in.songPosition",
            "out.clock", "out.startStop", "out.noteCc", "out.channelMapReset", "out.songPosition", "out.channelMap"]
    if midi:
        rows.append("syncSource")
    rows += ["midiPorts"]  # LINK SETTING: disabled until SYNC SOURCE offers LINK (P2)
    rows += ["padVelocity", "padCurve", "padThreshold", "padSensitivity", "padModeSelectResponse", "muteMode",
             "metronomeMonitor",
             "metronomeVolume", "countIn", "brightness", "audioDevice", "outputMap"]
    if ctrl:
        rows += ["controllers"] + ([] if DEVICE else ["keyboardMap"])
    rows += ["sceneBpm", "resume", "autosave", "theme", "uiScale", "envAutoZoom", "patternSwitch", "songEnd", "stripActsOn",
             "fxLfoRestart", "extensions", "libraryLocations", "version", "diskSpace"]
    return rows


# --- the generator ----------------------------------------------------------------------------------------------------
class Gen:
    def __init__(self, seed: int, minutes: float):
        self.rng = random.Random(seed)
        self.seed, self.minutes = seed, minutes
        self.t = 0
        self.ev = []          # (ms, order, line)
        self.order = 0
        self.routines = []    # {"name", "at", "features", "screens"}
        self.cur = None
        self.saves = []
        self.last_save_ms = -10 ** 9
        self.probes = []      # {"state": index of the `cmd state` checkpoint, "scene", "pattern", "cells", "features"}
        self.states = 0
        self.gaps = 0
        self.controls = set()
        # generator-side state (what the app's state is expected to be between routines)
        self.track = 1        # selected track
        self.mode = "play"    # pad mode
        self.shift = False
        self.playing = False
        self.bpm = 120.0
        self.pattern = 1      # current pattern of the current scene
        self.scene = 1
        self.length_bars = 1

    # low-level
    def at(self, ms, line):
        self.ev.append((int(ms), self.order, line))
        self.order += 1

    def wait(self, ms):
        self.t += int(ms)

    def press(self, name, value=None):
        self.controls.add(canonical(name))
        if name == "shift":
            self.shift = True
        elif name.startswith("padmode.") and not self.shift:
            m = name.split(".")[1]
            self.mode = "play" if self.mode == m else m   # one-hot; the lit key ends its mode (UI-104)
        elif name.startswith("pad") and name[3:].isdigit() and (self.mode in ("play", "track")
                                                                 or (self.shift and self.mode != "mute")):
            self.track = int(name[3:])                     # PLAY / TRACK mode and SHIFT + pad select (PAD-004)
        self.at(self.t, f"press {name}" + (f" {value}" if value is not None else ""))

    def release(self, name):
        if name == "shift":
            self.shift = False
        self.at(self.t, f"release {name}")

    def key(self, name, gap=110, hold=50, value=None):
        self.press(name, value)
        self.at(self.t + hold, f"release {name}")
        self.t += gap

    def turn(self, name, detents, gap=70):
        self.controls.add(canonical(name))
        self.at(self.t, f"turn {name} {detents:+d}")
        self.t += gap

    def strip(self, pos, gap=40):
        self.controls.add("strip")
        self.at(self.t, f"strip {pos}" if pos != "off" else "strip off")
        self.t += gap

    def cmd(self, text, gap=200, gap_note=None):
        if text == "state":
            self.states += 1
        if gap_note:
            self.gaps += 1
            self.at(self.t, f"cmd {text}  # GAP (NFR-040): {gap_note}")
        else:
            self.at(self.t, f"cmd {text}")
        self.t += gap

    def shift_key(self, name, gap=150):
        self.press("shift")
        self.t += 30
        self.key(name, gap=60)
        self.release("shift")
        self.t += gap

    def pad(self, n, vel=None, hold=70, gap=None):
        v = vel if vel is not None else self.rng.randint(70, 127)
        self.key(f"pad{n}", gap=gap if gap is not None else hold + 30, hold=hold, value=v)

    # bookkeeping
    def begin(self, name):
        self.cur = {"name": name, "at": self.t, "features": [], "screens": []}
        self.routines.append(self.cur)

    def feat(self, *names):
        for n in names:
            if n not in self.cur["features"]:
                self.cur["features"].append(n)

    def screen(self, sid):
        if sid not in self.cur["screens"]:
            self.cur["screens"].append(sid)

    def step_ms(self):
        return 60000.0 / self.bpm / 4

    def bar_ms(self):
        return self.step_ms() * 16 * self.length_bars

    # navigation helpers (panel paths)
    def home(self, gap=250):
        self.key("home", gap=gap)

    def track_menu(self):
        self.key("sel.push", gap=350)   # HOME: push = TRACK MENU of the selected track (UI-401)
        self.screen("track.menu")
        self.feat("nav.trackMenu")

    def open_module(self, index):
        # TRACK MENU (Sample track): -20 clamps the focus to LIVE SAMPLING, then +index:
        # 1 BROWSE, 2 SAMPLING/EDIT, 3 PLAYBACK, 4 AMP ENVELOPE, 5 FX1, 6 FX2, 7 LFO (MIXER is disabled), 8 SEQUENCE
        self.turn("sel", -20)
        self.turn("sel", index)
        self.key("sel.push", gap=400)

    def select_track(self, n):
        # HOME: the selector moves the selection (wraps 16 -> 1); pads would also play
        d = n - self.track
        if d:
            self.turn("sel", d, gap=120)
        self.track = n
        self.feat("track.selectSelector")

    # --- routines (each starts and ends on HOME, pad mode PLAY, SCENE / PATTERN modes off, strip lifted) ---------------
    def r_start(self):
        self.begin("start")
        self.key("play", gap=300)   # PLAY: the current pattern (1 of scene 1) from the top
        self.playing = True
        self.feat("transport.play")

    def r_pads(self):
        self.begin("pads")
        n = self.rng.randint(12, 32)
        spacing = max(60, int(self.step_ms()))
        for _ in range(n):
            self.pad(self.rng.randint(1, 16), hold=40, gap=spacing * self.rng.choice((1, 1, 2)))
        self.feat("pad.play")

    def r_steps(self):
        self.begin("steps")
        self.select_track(self.rng.randint(1, 8))
        for s in self.rng.sample(range(1, 17), self.rng.randint(3, 6)):
            self.key(f"step{s}", gap=140, hold=60)   # step entry: a short press toggles a trigger (SEQ-040)
        self.feat("step.entry")
        self.key("ext.undo", gap=200)        # Cmd+Z: the last step toggle undone …
        self.key("ext.redo", gap=200)        # … Shift+Cmd+Z: redone
        self.feat("edit.undo", "edit.redo")
        if self.rng.random() < 0.5:          # Delete with a step held: clear that step (UI-120, SEQ-012)
            s = self.rng.randint(1, 16)
            self.press(f"step{s}")
            self.t += 100
            self.key("ext.clear", gap=100)
            self.release(f"step{s}")
            self.t += 150
            self.feat("step.clear")
        # hold + turn: a step lock (SEQ-022; HOME's strip: VOLUME, PAN, PITCH)
        for _ in range(self.rng.randint(1, 3)):
            s = self.rng.randint(1, 16)
            knob = self.rng.randint(1, 3)
            self.press(f"step{s}")
            self.t += 120
            self.turn(f"knob{knob}", self.rng.choice((-3, -2, 2, 3)), gap=120)
            self.release(f"step{s}")
            self.t += 150
        self.feat("step.lock")
        if self.length_bars > 1:
            self.key("bar2", gap=150)
            self.key(f"step{self.rng.randint(1, 16)}", gap=140)
            self.key("bar1", gap=150)
            self.feat("bar.select")

    def r_home_knobs(self):
        self.begin("home-knobs")
        for knob in (1, 2, 3):
            d = self.rng.randint(1, 4) * self.rng.choice((-1, 1))
            self.turn(f"knob{knob}", d, gap=120)
            self.turn(f"knob{knob}", -d, gap=120)
        self.press("shift")
        self.turn("knob1", 3, gap=80)    # SHIFT + knob = fine step (UI-111)
        self.turn("knob1", -3, gap=80)
        self.release("shift")
        self.t += 100
        self.feat("knob.home", "knob.fine")

    def r_pad_modes(self):
        self.begin("pad-modes")
        # TRACK pad mode: pads select, no sound
        self.key("padmode.track", gap=200)
        self.pad(self.rng.randint(1, 8), gap=200)
        self.key("padmode.track", gap=200)  # the lit key ends the mode (UI-104)
        self.feat("pad.track")
        # SHIFT + pad in play mode selects without sound (PAD-004)
        t = self.rng.randint(1, 8)
        self.press("shift")
        self.t += 30
        self.pad(t, gap=60)
        self.release("shift")
        self.t += 150
        self.feat("track.selectShiftPad")
        # SCALE pad mode: the selected track at the pads' scale degrees
        self.key("padmode.scale", gap=200)
        for _ in range(self.rng.randint(8, 16)):
            self.pad(self.rng.randint(1, 16), hold=60, gap=int(self.step_ms()))
        self.key("padmode.scale", gap=200)
        self.feat("pad.scale")
        # SLICE pad mode: the selected track's slices
        self.key("padmode.slice", gap=200)
        for _ in range(self.rng.randint(8, 16)):
            self.pad(self.rng.randint(1, 16), hold=60, gap=int(self.step_ms()))
        self.key("padmode.slice", gap=200)
        self.feat("pad.slice")

    def r_mute_solo(self):
        self.begin("mute-solo")
        self.key("padmode.mute", gap=200)
        a = self.rng.randint(1, 8)
        self.pad(a, gap=600)        # mute track a
        self.pad(a, gap=300)        # unmute
        self.feat("pad.mute")
        b = self.rng.randint(1, 8)
        self.press("shift")
        self.t += 30
        self.pad(b, gap=60)         # SHIFT + pad: mute every other track (solo b)
        self.release("shift")
        self.t += 1200
        self.press("shift")
        self.t += 30
        self.pad(b, gap=60)         # the same again: unmute all
        self.release("shift")
        self.t += 200
        self.key("padmode.mute", gap=200)
        self.feat("pad.solo")
        c = self.rng.randint(9, 16)
        self.press("padmode.mute")  # hold MUTE + pad: SOLO on, off; the release goes back to PLAY
        self.t += 60
        self.pad(c, gap=300)
        self.pad(c, gap=300)
        self.release("padmode.mute")
        self.mode = "play"          # a pad was used: the release restores the mode the press found
        self.t += 200
        self.feat("pad.soloHold")

    def r_lfo(self):
        self.begin("lfo")
        self.track_menu()
        self.open_module(7)
        self.screen("track.lfo")
        self.feat("nav.lfo")
        for knob in (1, 2, 3):               # SPEED, DEPTH, PHASE
            self.turn(f"knob{knob}", self.rng.randint(1, 6) * self.rng.choice((-1, 1)), gap=120)
        self.turn("knob4", self.rng.choice((1, 2, 3)), gap=150)   # DESTINATION
        self.turn("knob5", self.rng.choice((-1, 1)), gap=150)     # SHAPE
        self.feat("lfo.knobs")
        # the rail with the selector: -10, -10 = DESTINATION from any remembered focus
        self.turn("sel", -10)
        self.turn("sel", -10)
        self.key("sel.push", gap=350)        # DESTINATION -> the target list
        self.turn("sel", self.rng.randint(1, 4), gap=120)
        self.key("sel.push", gap=300)        # choose
        self.feat("lfo.destination")
        self.turn("sel", 1)                  # SHAPE
        self.key("sel.push", gap=350)
        self.turn("sel", self.rng.randint(1, 3), gap=120)
        self.key("sel.push", gap=300)
        self.feat("lfo.shape")
        self.turn("sel", 1)                  # SETTING
        self.key("sel.push", gap=400)
        self.key("back", gap=300)            # closes the popup (BACK closes a popup first)
        self.feat("lfo.setting")
        self.turn("sel", 1)                  # SYNC
        self.key("sel.push", gap=200)
        self.feat("lfo.sync")
        # a step lock of an LFO parameter (hold step + turn SPEED)
        s = self.rng.randint(1, 16)
        self.press(f"step{s}")
        self.t += 120
        self.turn("knob1", self.rng.choice((-4, 4)), gap=120)
        self.release(f"step{s}")
        self.t += 150
        self.feat("lfo.lock")
        self.home()

    def r_amp(self):
        self.begin("amp-envelope")
        self.track_menu()
        self.open_module(4)
        self.screen("track.ampEnvelope")
        self.feat("nav.ampEnvelope")
        for knob in (2, 3, 4):               # ATTACK, HOLD, RELEASE
            d = self.rng.randint(1, 5) * self.rng.choice((-1, 1))
            self.turn(f"knob{knob}", d, gap=120)
        self.turn("sel", 1)                  # the active handle
        self.key("sel.push", gap=400)        # SETTING popup
        self.key("back", gap=300)
        self.feat("amp.knobs", "amp.setting")
        # the rail: SHIFT + push gives it the focus (SETTING; slot 1 is the PRESET hole), +1 = SYNC, push toggles it;
        # past SYNC the rotary returns to the handles (the focus the next visit starts with)
        self.shift_key("sel.push", gap=200)
        self.turn("sel", 1, gap=120)
        self.key("sel.push", gap=250)        # SYNC on
        self.key("sel.push", gap=250)        # SYNC off
        self.turn("sel", 1, gap=150)
        self.feat("amp.sync")
        self.home()

    def r_sequence(self):
        self.begin("sequence")
        self.track_menu()
        self.open_module(8)
        self.screen("track.sequence")
        self.feat("nav.sequence")
        targets = self.rng.sample(range(1, 17), self.rng.randint(1, 3))
        for s in targets:
            self.key(f"step{s}", gap=150)    # target steps (UI-246)
        self.feat("seq.targets")
        self.turn("knob2", self.rng.randint(1, 4), gap=150)     # RETRIGGER CNT (ratchets, SEQ-030)
        self.turn("knob3", self.rng.choice((-1, 1)), gap=150)   # RETRIGGER SPD
        self.feat("seq.ratchet")
        for s in targets:
            self.key(f"step{s}", gap=150)    # targets off: the track-level values
        d = self.rng.randint(1, 5) * self.rng.choice((-1, 1))
        self.turn("knob6", d, gap=200)       # TRANSPOSE (SEQ-035)
        self.feat("seq.transpose")
        self.turn("knob5", self.rng.choice((-1, 1)), gap=150)   # SHIFT AREA
        sh = self.rng.choice((-2, -1, 1, 2))
        self.turn("knob4", sh, gap=250)      # SHIFT: rotate the steps (SEQ-036)
        self.turn("knob4", -sh, gap=250)
        self.feat("seq.shift")
        self.turn("knob6", -d, gap=200)
        self.home()

    def r_set_slice(self):
        self.begin("set-slice")
        self.key("padmode.slice", gap=200)
        self.shift_key("padmode.slice", gap=450)   # SHIFT + the lit SLICE key = SET SLICE (UI-403)
        self.screen("track.playback.setSlice")
        self.feat("nav.setSlice")
        # the rail holds the rotary on entry (ADD SLICE): -10 = ADD SLICE from any remembered focus
        self.turn("sel", -10, gap=120)
        self.key("sel.push", gap=250)        # ADD SLICE (the new slice is selected)
        self.turn("sel", 1, gap=120)
        self.key("sel.push", gap=250)        # DELETE SLICE (it)
        self.turn("sel", 1, gap=120)
        self.key("sel.push", gap=350)        # GRID SLICE: the list, the current count focused
        self.key("sel.push", gap=300)        # the same count (no moved points: nothing to reset)
        self.turn("sel", 1, gap=120)         # past GRID SLICE: the rotary zooms the waveform
        self.feat("slice.add", "slice.delete", "slice.grid")
        self.turn("knob1", self.rng.randint(1, 5), gap=150)  # SLICE No.
        self.turn("knob5", 1, gap=150)       # AREA
        self.turn("knob5", -1, gap=150)
        self.turn("sel", 2, gap=120)         # zoom in time
        self.turn("sel", -2, gap=120)
        self.feat("slice.edit")
        for _ in range(6):
            self.pad(self.rng.randint(1, 16), hold=60, gap=int(self.step_ms()))
        self.home()
        self.key("padmode.slice", gap=200)

    def r_set_scale(self):
        self.begin("set-scale")
        self.track_menu()
        self.open_module(3)                  # PLAYBACK (panel)
        self.screen("track.playback")
        self.feat("nav.playback")
        # PLAYBACK's rail (SET SLICE, SET SCALE, REVERSE, GRID SNAP): the rotary zooms the waveform, SHIFT + push gives
        # the rail the focus (SET SLICE); REVERSE and GRID SNAP toggled on and off again; SET SCALE opens
        self.shift_key("sel.push", gap=200)
        self.turn("sel", 2, gap=120)         # REVERSE
        self.key("sel.push", gap=200)
        self.key("sel.push", gap=250)
        self.turn("sel", 1, gap=120)         # GRID SNAP
        self.key("sel.push", gap=200)
        self.key("sel.push", gap=250)
        self.feat("playback.reverse", "playback.gridSnap")
        self.turn("sel", -2, gap=120)        # SET SCALE
        self.key("sel.push", gap=400)
        self.screen("track.playback.setScale")
        self.feat("nav.setScale")
        self.turn("sel", 1)                  # the rail: SCALE
        self.key("sel.push", gap=350)        # the SCALE list
        self.turn("sel", self.rng.randint(1, 5), gap=120)
        self.key("sel.push", gap=300)
        self.turn("knob3", self.rng.choice((-1, 1)), gap=150)   # KEY SHIFT
        self.feat("scale.edit")
        self.key("back", gap=300)            # PLAYBACK (the rail keeps SET SCALE focused, UI-009) …
        self.turn("sel", 5, gap=150)         # … past GRID SNAP: the waveform has the rotary again
        self.home()
        # SHIFT + the lit SCALE key opens SET SCALE directly (UI-403); the SCALE mode stays on
        self.key("padmode.scale", gap=200)
        self.shift_key("padmode.scale", gap=450)
        self.feat("nav.setScaleShift")
        for _ in range(8):
            self.pad(self.rng.randint(1, 16), hold=60, gap=int(self.step_ms()))
        self.key("padmode.scale", gap=200)
        self.home()

    def r_strip_setting(self):
        self.begin("strip-setting")
        self.shift_key("home", gap=400)      # SHIFT + HOME = UTILITY (UI-103)
        self.screen("utility")
        self.feat("nav.utility")
        self.turn("sel", -80, gap=120)       # the first row (UTILITY remembers its focus)
        self.key("sel.push", gap=400)        # row 1: TOUCH STRIP SETTING (USER1)
        self.screen("utility.touchStrip.user1")
        self.feat("nav.touchStripUser1")
        place = self.rng.randint(0, 1)
        if place:
            self.turn("sel", place, gap=120)
        self.key("sel.push", gap=400)        # SELECT PARAMETER (an empty place opens on the first PLAYBACK row)
        self.turn("sel", self.rng.randint(1, 2), gap=120)
        self.key("sel.push", gap=300)        # assign
        self.turn("knob1", -self.rng.randint(1, 4), gap=150)    # BOTTOM VALUE
        self.turn("knob2", self.rng.randint(1, 4), gap=150)     # TOP VALUE
        self.feat("strip.assignUser1")
        self.key("back", gap=350)            # back to UTILITY
        self.turn("sel", 1, gap=120)         # row 2: USER2
        self.key("sel.push", gap=400)
        self.screen("utility.touchStrip.user2")
        self.feat("nav.touchStripUser2")
        self.key("sel.push", gap=400)
        self.turn("sel", 2, gap=120)
        self.key("sel.push", gap=300)
        self.turn("knob2", -2, gap=150)
        self.feat("strip.assignUser2")
        self.key("back", gap=350)            # UTILITY, on the USER2 row
        self.turn("sel", 1, gap=120)         # row 3: TOUCH STRIP SETTING (PITCH RANGE)
        self.key("sel.push", gap=350)        # its list (13 entries: a ListPopup, utility.md)
        self.turn("sel", -20, gap=120)       # 1 Bipolar Semitone
        self.turn("sel", self.rng.randint(2, 4), gap=120)
        self.key("sel.push", gap=300)        # 3 … 5 Bipolar Semitones
        self.feat("strip.pitchRange")
        self.home()
        self.cmd("ribbon", gap=150)          # checkpoint: RIBBON STATE … pitch=… items=…

    def r_settings(self):
        """UTILITY and every sub-page by panel only (NFR-040): the rotary walks the rows, push edits a
        value (turn, push = commit; BACK = cancel) or opens a page, BACK returns to the row. A project setting (PITCH
        RANGE, checked in the RIBBON STATE checkpoint) and a preference (UI SCALE, checked in the PREFS checkpoint) are
        changed and put back; the CHANNEL MAPPING list popup and the RESET confirmation are opened and cancelled."""
        self.begin("settings")
        rows = utility_rows()
        at = [0]

        def go(name):
            i = rows.index(name)
            if i != at[0]:
                self.turn("sel", i - at[0], gap=120)
            at[0] = i

        def page(row, sid, feat):
            go(row)
            self.key("sel.push", gap=400)
            self.screen(sid)
            self.feat(feat)
            return sid

        def edit(row, detents):
            go(row)
            self.key("sel.push", gap=200)    # the value cell (editing)
            self.turn("sel", detents, gap=150)
            self.key("sel.push", gap=250)    # commit

        self.shift_key("home", gap=400)
        self.screen("utility")
        self.feat("nav.utility")
        self.turn("sel", -80, gap=120)       # row 1 (the focus is remembered between visits)
        # a project setting: PITCH RANGE (13 values: its ListPopup) to 1 Bipolar Semitone, checkpoint, then 2
        edit("pitchRange", -20)
        self.cmd("ribbon", gap=200)
        self.feat("settings.projectValue")
        go("pitchRange")
        self.key("sel.push", gap=200)
        self.turn("sel", -20, gap=120)
        self.turn("sel", 1, gap=120)
        self.key("sel.push", gap=250)
        # editing cancelled with BACK (PATTERN QUANTIZE stays PATTERN)
        go("patternQuantize")
        self.key("sel.push", gap=200)
        self.turn("sel", -1, gap=150)
        self.key("back", gap=250)
        self.feat("settings.cancel")
        # CHANNEL MAPPING (MIDI IN): the 17-value list of TRACK 2 opens and is cancelled; BACK to the row
        page("in.channelMap", "utility.channelMapIn", "nav.channelMapIn")
        self.turn("sel", 1, gap=150)
        self.key("sel.push", gap=300)        # ListPopup OFF, CH 1 ... CH 16
        self.key("back", gap=250)            # cancelled
        self.key("back", gap=350)            # UTILITY, on CHANNEL MAPPING
        go("in.channelMapReset")
        self.key("sel.push", gap=300)        # "Reset the MIDI IN channel mapping?" (CANCEL focused)
        self.key("sel.push", gap=300)        # CANCEL
        self.feat("settings.reset")
        page("out.channelMap", "utility.channelMapOut", "nav.channelMapOut")
        self.key("back", gap=350)
        page("midiPorts", "utility.midiPorts", "nav.midiPorts")
        self.key("back", gap=350)
        page("audioDevice", "utility.audioDevice", "nav.audioDevice")
        self.turn("sel", 2, gap=120)         # BUFFER SIZE: edit and cancel (applied at the next start)
        self.key("sel.push", gap=200)
        self.turn("sel", 1, gap=150)
        self.key("back", gap=250)
        self.key("back", gap=350)
        page("outputMap", "utility.outputMap", "nav.outputMap")
        self.key("back", gap=350)
        if "controllers" in rows:
            page("controllers", "utility.controllers", "nav.controllers")
            self.key("back", gap=350)
            if not DEVICE:
                page("keyboardMap", "utility.keyboardMap", "nav.keyboardMap")
                self.key("back", gap=350)
        # a preference: UI SCALE 100 -> 125 -> 100 (saved in preferences.json, the PREFS checkpoint shows it)
        edit("uiScale", 1)
        edit("uiScale", -1)
        self.feat("settings.preference")
        page("libraryLocations", "utility.library", "nav.library")
        self.key("back", gap=350)
        go("diskSpace")                      # INFORMATION: the end of the list
        self.key("sel.push", gap=200)        # info rows: push does nothing
        self.turn("sel", -80, gap=120)       # leave UTILITY on row 1
        self.home()
        self.cmd("prefs", gap=200)           # checkpoint: PREFS … ui.scale=100

    def r_strip(self):
        self.begin("strip")
        # PITCH (default mode): a pad, then the strip bends the selected track
        p = self.rng.randint(1, 8)
        self.pad(p, hold=60, gap=80)
        for pos in (64, 80, 100, 120, 90, 64, 40, 20, 64):
            self.strip(pos, gap=45)
        self.strip("off", gap=200)
        self.feat("strip.pitch")
        # REPEAT: hold a pad, the strip sets the repeat rate
        self.key("strip.mode", gap=200)
        self.press(f"pad{p}", 100)
        self.t += 60
        for pos in (30, 50, 70, 90):
            self.strip(pos, gap=350)
        self.strip("off", gap=100)
        self.release(f"pad{p}")
        self.t += 200
        self.feat("strip.repeat")
        # USER1 and USER2: the assigned parameters, swept; HOLD keeps the last value after the finger lifts
        for mode in ("strip.user1", "strip.user2"):
            self.key("strip.mode", gap=200)
            for _ in range(4):
                self.pad(self.rng.randint(1, 8), hold=50, gap=60)
                self.strip(self.rng.randint(0, 127), gap=180)
            self.strip("off", gap=150)
            self.feat(mode)
        self.key("strip.hold", gap=150)
        self.strip(self.rng.randint(20, 110), gap=300)
        self.strip("off", gap=500)           # held: the value stays
        self.key("strip.hold", gap=200)      # HOLD off: the strip lets go
        self.feat("strip.hold")
        self.key("strip.mode", gap=200)      # back to PITCH

    def r_scene_pattern(self):
        self.begin("scene-pattern")
        target = self.rng.choice([p for p in (1, 2, 3) if p != self.pattern])
        self.key("pattern", gap=150)
        self.key(f"step{target}", gap=150)   # queued to the PATTERN QUANTIZE boundary while playing
        self.key("pattern", gap=150)
        self.pattern = target
        self.feat("pattern.switch")
        self.wait(self.bar_ms() + 600)
        # SCENE mode: a scene key, then a pattern key commits scene + pattern
        scene = 2 if self.scene == 1 else 1
        self.key("scene", gap=150)
        self.key(f"step{scene}", gap=150)
        self.key("step1", gap=150)
        self.key("scene", gap=150)
        self.scene, self.pattern = scene, 1
        self.feat("scene.switch")
        self.wait(self.bar_ms() + 600)
        if self.scene != 1:                  # back to scene 1 (most routines play there)
            self.key("scene", gap=150)
            self.key("step1", gap=150)
            self.key("step1", gap=150)
            self.key("scene", gap=150)
            self.scene, self.pattern = 1, 1
            self.wait(self.bar_ms() + 600)

    def r_chain(self):
        self.begin("chain")
        if self.pattern != 1:
            self.key("pattern", gap=150)
            self.key("step1", gap=150)
            self.key("pattern", gap=150)
            self.pattern = 1
            self.wait(self.bar_ms() + 600)
        # PATTERN mode: hold the current pattern's key and press others: the chain 1 -> 2 -> 2 -> 3 (SCN-030)
        self.key("pattern", gap=150)
        self.press("step1")
        self.t += 100
        for k in (2, 2, 3):
            self.key(f"step{k}", gap=120)
        self.release("step1")
        self.t += 150
        self.key("pattern", gap=150)
        self.feat("pattern.chain")
        self.wait(4 * self.bar_ms() + 500)
        # a plain selection ends the chain: pattern 4, then 1 again
        self.key("pattern", gap=150)
        self.key("step4", gap=150)
        self.key("pattern", gap=150)
        self.wait(self.bar_ms() + 600)
        self.key("pattern", gap=150)
        self.key("step1", gap=150)
        self.key("pattern", gap=150)
        self.pattern = 1
        self.wait(self.bar_ms() + 600)

    def r_length(self):
        self.begin("pattern-length")
        bars = 2 if self.length_bars == 1 else 1
        self.shift_key("pattern", gap=150)   # SHIFT + PATTERN: pattern-length mode
        self.key(f"bar{bars}", gap=150)
        self.key("pattern", gap=200)
        self.length_bars = bars
        self.feat("pattern.length")
        self.cmd("state", gap=150)

    def r_copy_paste(self, probe=False):
        self.begin("copy-paste")
        if probe:
            # the deterministic probe (cycle 1): track 9 (cycle 1 plays nothing else on it), steps 1 and 3 entered on the
            # current pattern, copied and pasted at step 9: a `cmd state` checkpoint must show F1.9, F3.9, F9.9, F11.9
            self.key("padmode.track", gap=200)   # TRACK pad mode: pad 9 selects track 9 (no sound)
            self.feat("pad.track", "step.entry")
            self.pad(9, gap=200)
            self.key("padmode.track", gap=200)
            for s in (1, 3):
                self.key(f"step{s}", gap=160, hold=60)
            src = [1, 3]
        else:
            src = self.rng.sample(range(1, 9), 2)
        self.press("ext.copy")               # hold COPY + step keys (EDT-005)
        self.t += 80
        for s in src:
            self.key(f"step{s}", gap=120)
        self.release("ext.copy")
        self.t += 250
        self.feat("edit.copySteps")
        self.press("ext.paste")              # hold PASTE + the target step
        self.t += 80
        self.key(f"step{9 if probe else self.rng.randint(9, 16)}", gap=120)
        self.release("ext.paste")
        self.t += 600                        # the paste is prepared on a worker, installed on the UI thread
        self.feat("edit.pasteSteps")
        if probe:
            self.probes.append({"state": self.states, "scene": self.scene, "pattern": self.pattern,
                                "cells": ["F1.9", "F3.9", "F9.9", "F11.9"],
                                "features": ["pad.track", "step.entry", "edit.copySteps", "edit.pasteSteps"]})
            self.cmd("state", gap=200)

    def r_tempo(self):
        self.begin("tempo")
        self.bpm = float(self.rng.choice((96, 104, 110, 118, 124, 130, 138)))
        gap = 60000.0 / self.bpm
        t0 = self.t
        for k in range(5):                   # TAP (ext.tap, key N): the tempo from the taps' spacing (SYN-003)
            self.t = int(t0 + k * gap)
            self.key("ext.tap", gap=0, hold=40)
        self.t = int(t0 + 5 * gap)
        self.feat("tempo.tap")

    def r_record(self):
        self.begin("record")
        self.key("rec", gap=150)             # REC armed while playing: hits are recorded into the pattern
        n = self.rng.randint(6, 12)
        for _ in range(n):
            self.pad(self.rng.randint(1, 8), hold=50, gap=int(self.step_ms() * self.rng.choice((1, 2))))
        self.wait(300)
        self.key("rec", gap=200)             # REC off: the pass is one undo step
        self.feat("rec.pass")

    def r_transport(self):
        self.begin("transport")
        self.key("play", gap=900)            # PLAY while playing = pause
        self.key("play", gap=900)            # continue
        self.feat("transport.pause")
        self.key("stop", gap=700)            # stop with tails, back to the start
        self.key("play", gap=400)
        self.feat("transport.stop")

    def r_erase(self):
        self.begin("erase")
        self.press("ext.erase")              # held ERASE (key J): the playing steps of the selected track (SEQ-046)
        self.t += int(self.step_ms() * 3)
        self.release("ext.erase")
        self.t += 200
        self.feat("seq.erase")

    def r_select(self):
        self.begin("select")
        self.select_track(self.rng.randint(1, 16))
        self.wait(200)
        self.select_track(self.rng.randint(1, 8))
        self.wait(200)

    def r_save(self):
        # The script is time-based: a save may still run (a slow machine, a first save copying the kit) when the next
        # one would start, and the store refuses an overlapping one (correctly). Saves are spaced >= SAVE_SPACING_MS.
        self.t = max(self.t, self.last_save_ms + SAVE_SPACING_MS)
        self.last_save_ms = self.t
        self.begin("save")
        self.saves.append(f"save-{len(self.saves) + 1}")
        self.key("ext.project", gap=400)     # Cmd+4 = PROJECT (UI-102)
        self.screen("project")
        self.feat("nav.project")
        self.turn("sel", -20, gap=100)       # CREATE NEW …
        self.turn("sel", 2, gap=150)         # … SAVE
        self.key("sel.push", gap=600)        # the first save asks for the name, later ones OVERWRITE?
        self.key("sel.push", gap=2500)       # SAVE / OVERWRITE (the blocking progress, then the toast)
        self.feat("project.save")
        self.home(gap=600)
        self.home()                          # (a progress still open ignores the first HOME)

    def r_scene_manager(self):
        self.begin("scene-manager")
        self.shift_key("scene", gap=500)     # SHIFT + SCENE = SCENE MANAGER (UI-403)
        self.screen("sceneManager")
        self.feat("nav.sceneManager")
        self.turn("sel", 1, gap=200)
        self.turn("sel", -1, gap=200)
        # COPY / PASTE / DELETE on the keys (Cmd+C, Cmd+V, Delete), on the empty scenes 15-16 the set never plays:
        # scene 16 copied, pasted onto 15, 15 deleted (it asks: CANCEL focused, +1 = DELETE)
        self.turn("sel", 16, gap=150)        # scene 16 (the focus clamps)
        self.key("ext.copy", gap=300)
        self.turn("sel", -1, gap=150)
        self.key("ext.paste", gap=700)
        self.key("ext.clear", gap=300)
        self.turn("sel", 1, gap=150)
        self.key("sel.push", gap=400)
        # the patterns of scene 15: 16 copied, pasted onto 15, 15 deleted (not playing: at once)
        self.key("sel.push", gap=200)
        self.turn("sel", 16, gap=150)
        self.key("ext.copy", gap=300)
        self.turn("sel", -1, gap=150)
        self.key("ext.paste", gap=700)
        self.key("ext.clear", gap=400)
        self.feat("edit.sceneCopy", "edit.scenePaste", "edit.sceneDelete", "edit.patternCopy", "edit.patternPaste",
                  "edit.patternDelete")
        self.shift_key("scene", gap=300)     # SHIFT + SCENE again closes it (nothing committed)

    def r_menus(self):
        self.begin("menus")
        # the extension jump keys (UI-102, UI-403) and their SHIFT neighbours (our choice)
        for name, shift, sid in (("ext.mixer", False, "mixer"), ("ext.mixer", True, "arranger"),
                                 ("ext.utility", True, "trackSetting"), ("ext.utility", False, "utility"),
                                 ("ext.browse", False, "track.browse")):
            if shift:
                self.shift_key(name, gap=400)
            else:
                self.key(name, gap=400)
            self.screen(sid)
        self.feat("nav.mixer", "nav.arranger", "nav.trackSetting", "nav.browseKey")
        self.home()
        self.shift_key("sel.push", gap=400)  # HOME: SHIFT + push = BROWSE of the selected track (UI-403)
        self.feat("nav.browseShiftPush")
        self.home()

    def r_bpm(self):
        self.begin("bpm-quantize")
        self.shift_key("ext.tap", gap=400)   # SHIFT + TAP = the BPM/QUANTIZE popup (our choice; UI-408)
        self.feat("tempo.popup")
        for d in (2, 3, 1):                  # BPM, SWING, QUANTIZE: changed and back; push = the next field
            self.turn("sel", d, gap=150)
            self.turn("sel", -d, gap=150)
            self.key("sel.push", gap=150)
        self.feat("tempo.fields")
        self.shift_key("sel.push", gap=800)  # SHIFT + push: the metronome on …
        self.shift_key("sel.push", gap=200)  # … and off
        self.feat("tempo.metronome")
        self.press("ext.nudgeUp")            # NUDGE held (key ', our choice) in the popup …
        self.t += 300
        self.release("ext.nudgeUp")
        self.t += 200
        self.key("back", gap=300)            # BACK closes the popup
        self.press("ext.nudgeDown")          # … and anywhere (key ;)
        self.t += 250
        self.release("ext.nudgeDown")
        self.t += 200
        self.feat("tempo.nudge")

    # the sampling screens through the TRACK MENU header (panel + keyboard; no REC: the set's audio and
    # its stuck-note check stay the kit's).
    def r_sampling_edit(self):
        self.begin("sampling-edit")
        self.track_menu()
        self.open_module(2)                  # TRACK MENU header: SAMPLING/EDIT
        self.screen("sampling.edit")
        self.feat("nav.samplingEdit")
        self.turn("knob6", -2, gap=150)      # MONITOR LEVEL (editable, column 6)
        self.turn("knob6", 2, gap=150)
        self.feat("sampling.knobs")
        self.key("back", gap=300)            # no unsaved take: BACK leaves at once
        self.home()

    def r_live_sampling(self):
        self.begin("live-sampling")
        self.track_menu()
        self.open_module(0)                  # TRACK MENU header: LIVE SAMPLING
        self.screen("sampling.live")
        self.feat("nav.liveSampling")
        self.turn("knob2", 1, gap=150)       # LENGTH
        self.turn("knob2", -1, gap=150)
        self.feat("live.knobs")
        self.key("back", gap=300)
        self.home()

    def r_checkpoint(self):
        self.begin("checkpoint")
        self.cmd("state", gap=200)

    # --- the session ----------------------------------------------------------------------------------------------
    def build(self):
        self.t = 4000                       # the kit loads in < 1 s (KIT ready); 4 s margin
        self.r_start()
        first = [self.r_pads, self.r_steps, self.r_home_knobs, self.r_pad_modes, self.r_mute_solo, self.r_lfo,
                 self.r_amp, self.r_sequence, self.r_set_slice, self.r_set_scale, self.r_strip_setting, self.r_settings, self.r_strip,
                 self.r_length, self.r_steps, self.r_length, self.r_scene_pattern, self.r_chain,
                 lambda: self.r_copy_paste(True), self.r_tempo, self.r_record, self.r_transport, self.r_erase,
                 self.r_select, self.r_scene_manager, self.r_menus, self.r_bpm, self.r_sampling_edit,
                 self.r_live_sampling, self.r_save, self.r_checkpoint]
        for r in first:
            r()
            self.wait(400)
        self.cycle1_ms = self.t
        pool = [(self.r_pads, 10), (self.r_steps, 6), (self.r_home_knobs, 3), (self.r_pad_modes, 3),
                (self.r_mute_solo, 3), (self.r_lfo, 3), (self.r_amp, 2), (self.r_sequence, 3), (self.r_set_slice, 2),
                (self.r_set_scale, 1), (self.r_strip_setting, 1), (self.r_settings, 1), (self.r_strip, 4), (self.r_length, 1),
                (self.r_scene_pattern, 3), (self.r_chain, 1), (self.r_copy_paste, 3), (self.r_tempo, 2),
                (self.r_record, 3), (self.r_transport, 2), (self.r_erase, 1), (self.r_select, 2),
                (self.r_scene_manager, 1), (self.r_menus, 1), (self.r_bpm, 1), (self.r_sampling_edit, 1),
                (self.r_live_sampling, 1)]
        funcs = [f for f, _ in pool]
        weights = [w for _, w in pool]
        end = int(self.minutes * 60000)
        last_save, last_state = self.t, self.t
        while self.t < end - 60000:
            self.rng.choices(funcs, weights)[0]()
            self.wait(self.rng.randint(200, 1200))
            if self.t - last_save > 5 * 60000:
                self.r_save()
                last_save = self.t
            if self.t - last_state > 3 * 60000:
                self.r_checkpoint()
                last_state = self.t
        # the end: a last save, then STOP, STOP (stop with tails, then silence everything), checkpoints, quit
        self.t = max(self.t, end - 50000)
        if self.length_bars != 1:
            self.r_length()
        self.r_save()
        self.t = max(self.t, end - 12000)
        self.begin("end")
        self.key("stop", gap=500)
        self.key("stop", gap=300)
        self.playing = False
        self.feat("transport.stop", "transport.hardStop")
        self.stop_ms = self.t
        self.t = max(self.t, end - 6000)
        self.cmd("state", gap=200)
        self.cmd("ribbon", gap=200)
        self.t = max(self.t, end)
        self.quit_ms = self.t + 4000         # >= 4 s of silence after the hard stop
        self.at(self.quit_ms, "quit")

    def write(self, out, manifest):
        lines = [f"# generated by tools/liveset-check.py (live set without the mouse) seed={self.seed} "
                 f"minutes={self.minutes:g}; time 0 = the app's first frame",
                 "# controls: logical names (io::controlFromName); every one is a panel control or a UI-120 key"]
        for ms, _, line in sorted(self.ev):
            lines.append(f"{ms} {line}")
        with open(out, "w") as f:
            f.write("\n".join(lines) + "\n")
        features = []
        for r in self.routines:
            for x in r["features"]:
                if x not in features:
                    features.append(x)
        screens = []
        for r in self.routines:
            for s in r["screens"]:
                if s not in screens:
                    screens.append(s)
        m = {"seed": self.seed, "minutes": self.minutes, "events": len(self.ev), "cycle1_ms": self.cycle1_ms,
             "stop_ms": self.stop_ms, "quit_ms": self.quit_ms, "saves": self.saves, "gaps": self.gaps, "probes": self.probes,
             "features": features, "screens": screens, "controls": sorted(self.controls),
             "routines": [{"name": r["name"], "at": r["at"]} for r in self.routines]}
        with open(manifest, "w") as f:
            json.dump(m, f, indent=1)
        return m


def script_cmd(a) -> int:
    g = Gen(a.seed, a.minutes)
    g.build()
    m = g.write(a.out, a.manifest)
    counts = {}
    for r in g.routines:
        counts[r["name"]] = counts.get(r["name"], 0) + 1
    print(f"liveset script: seed={a.seed} minutes={a.minutes:g} events={m['events']} routines={len(g.routines)} "
          f"cycle1={m['cycle1_ms'] / 1000:.0f} s quit={m['quit_ms'] / 1000:.0f} s gaps(cmd)={m['gaps']} "
          f"features={len(m['features'])} screens={len(m['screens'])}")
    print("  routines: " + ", ".join(f"{k} x{v}" for k, v in counts.items()))
    return 0


# --- evidence in the log for each feature -----------------------------------------------------------------------------
EVIDENCE = {
    "transport.play": r"TRANSPORT play -> playing",
    "transport.pause": r"TRANSPORT play -> paused",
    "transport.stop": r"TRANSPORT stop -> stopped",
    "pad.play": r"INPUT pad\d+ down velocity=",
    "pad.track": None,  # the probe: TRACK mode + pad 9 selected track 9 (its entered steps are on track 9)
    "pad.scale": r"INPUT pad\d+ scale track=",
    "pad.slice": r"INPUT pad\d+ slice=([1-9]|1[0-6])\b",
    "pad.mute": r"INPUT pad\d+ mute mute=1 ",
    "pad.solo": r"INPUT pad\d+ mute-others mute=0 solo=\d muted=15 soloed=\d+$",
    "pad.soloHold": r"INPUT pad\d+ solo mute=\d solo=1 muted=\d+ soloed=1$",
    "track.selectSelector": r"INPUT|NAV",
    "track.selectShiftPad": r"INPUT pad\d+ down",
    "step.entry": None,  # the probe
    "transport.hardStop": None,  # two TRANSPORT stop lines in a row (stop with tails, then silence)
    "step.lock": r"INPUT knob[1-6] [+-]\d+ \((track|playback)\.[A-Za-z]+ = .* lock\)",
    "bar.select": r"LAMP .*bar\.2=",
    "knob.home": r"INPUT knob[1-3] [+-]\d+ \((track\.volume|track\.pan|playback\.pitch) = ",
    "knob.fine": r"INPUT knob1 [+-]3 \(track\.volume = ",
    "nav.trackMenu": r"NAV track\.menu ",
    "nav.lfo": r"NAV track\.lfo ",
    "lfo.knobs": r"INPUT knob[1-5] [+-]\d+ \(lfo\.(speed|depth|phase|destination|shape)",
    "lfo.destination": r"INPUT rail DESTINATION",
    "lfo.shape": r"INPUT rail SHAPE",
    "lfo.setting": r"INPUT rail SETTING",
    "lfo.sync": r"INPUT rail SYNC",
    "lfo.lock": r"INPUT knob1 [+-]\d+ \(lfo\.speed = .* lock\)",
    "nav.ampEnvelope": r"NAV track\.ampEnvelope ",
    "amp.knobs": r"INPUT knob[2-4] [+-]\d+ \(env\.(attack|hold|release)",
    "amp.setting": r"NAV track\.ampEnvelope ",
    "nav.sequence": r"NAV track\.sequence ",
    "seq.targets": r"NAV track\.sequence ",
    "seq.ratchet": r"INPUT knob2 [+-]\d+ \(seq\.retrigCount",
    "seq.transpose": r"INPUT knob6 [+-]\d+ \(seq\.transpose",
    "seq.shift": r"INPUT knob4 [+-]\d+ \(seq\.shift\b",
    "seq.erase": r"TRANSPORT|INPUT",
    "nav.setSlice": r"NAV track\.playback\.setSlice ",
    "slice.edit": r"INPUT knob1 [+-]\d+ \(slice\.selected",
    "nav.playback": r"NAV track\.playback ",
    "nav.setScale": r"NAV track\.playback\.setScale ",
    "scale.edit": r"INPUT knob3 [+-]\d+ \(scale\.keyShift",
    "nav.utility": r"NAV utility ",
    "nav.touchStripUser1": r"NAV utility\.touchStrip\.user1 ",
    "nav.touchStripUser2": r"NAV utility\.touchStrip\.user2 ",
    "nav.channelMapIn": r"NAV utility\.channelMapIn ",
    "nav.channelMapOut": r"NAV utility\.channelMapOut ",
    "nav.midiPorts": r"NAV utility\.midiPorts ",
    "nav.audioDevice": r"NAV utility\.audioDevice ",
    "nav.outputMap": r"NAV utility\.outputMap ",
    "nav.controllers": r"NAV utility\.controllers ",
    "nav.keyboardMap": r"NAV utility\.keyboardMap ",
    "nav.library": r"NAV utility\.library ",
    "settings.projectValue": r"RIBBON STATE .* pitch=-100\.\.100 ",
    "settings.preference": r"PREFS .*\bui\.scale=100\b",
    "settings.cancel": r"INPUT back \(popup\)",
    "settings.reset": r"NAV utility\.channelMapIn ",
    "strip.assignUser1": r"RIBBON STATE .*items=.*U1\.\d=",
    "strip.assignUser2": r"RIBBON STATE .*items=.*U2\.\d=",
    "strip.pitch": r"RIBBON touch \d+ source=\d+ mode=PITCH",
    "strip.repeat": r"RIBBON touch \d+ source=\d+ mode=REPEAT",
    "strip.user1": r"RIBBON touch \d+ source=\d+ mode=USER1",
    "strip.user2": r"RIBBON touch \d+ source=\d+ mode=USER2",
    "strip.hold": r"RIBBON hold mode=\w+ hold=1",
    "pattern.switch": r"M2 SWITCH scene=1 pattern=[2-9]",
    "scene.switch": r"M2 SWITCH scene=2 ",
    "pattern.chain": r"EDIT CHAIN scene=1 patterns=1,2,2,3",
    "pattern.length": r"M2 PATTERN scene=1 pattern=\d+ length=32 ",
    "edit.copySteps": None,  # the probe: steps 1, 3 copied …
    "edit.pasteSteps": None,  # … and pasted at step 9 (F9.9, F11.9)
    "tempo.tap": r"TRANSPORT ext\.tap -> ",
    "rec.pass": r"TRANSPORT rec -> playing .*rec=1",
    "nav.sceneManager": r"INPUT key shift\+scene -> sceneManager",
    "project.save": r"SAVED \S+ ok",
    # the paths that were NFR-040 gaps
    "nav.project": r"INPUT key ext\.project -> project",
    "amp.sync": r"INPUT rail SYNC",
    "slice.add": r"INPUT rail ADD SLICE",
    "slice.delete": r"INPUT rail DELETE SLICE",
    "slice.grid": r"INPUT rail GRID SLICE",
    "playback.reverse": r"INPUT rail REVERSE",
    "playback.gridSnap": r"INPUT rail GRID SNAP",
    "nav.setScaleShift": r"INPUT key shift\+padmode\.scale -> track\.playback\.setScale",
    "strip.pitchRange": r"RIBBON STATE .* pitch=-[3-5]00\.\.[3-5]00 ",
    "edit.undo": r"EDIT undo ok",
    "edit.redo": r"EDIT redo ok",
    "step.clear": r"EDIT clear step=\d+ track=\d+ ok",
    "edit.sceneCopy": r"TOAST Scene 16 copied",
    "edit.scenePaste": r"TOAST Scene 16 pasted to scene 15",
    "edit.sceneDelete": r"TOAST Scene 15 deleted",
    "edit.patternCopy": r"TOAST Pattern 16 copied",
    "edit.patternPaste": r"TOAST Pattern 15 pasted to scene 15",
    "edit.patternDelete": r"TOAST Pattern 15 deleted",
    "nav.mixer": r"INPUT key ext\.mixer -> mixer",
    "nav.arranger": r"INPUT key shift\+ext\.mixer -> arranger",
    "nav.trackSetting": r"INPUT key shift\+ext\.utility -> trackSetting",
    "nav.browseKey": r"INPUT key ext\.browse -> track\.browse",
    "nav.browseShiftPush": r"NAV track\.browse ",
    "tempo.popup": r"INPUT key shift\+ext\.tap -> bpmQuantize",
    "tempo.fields": r"INPUT sel [+-]\d+ \(popup\)",
    "tempo.metronome": r"INPUT sel\.push shift \(popup\)",
    "tempo.nudge": r"TRANSPORT ext\.nudge(Up|Down) -> ",
    "nav.samplingEdit": r"NAV sampling\.edit ",
    "sampling.knobs": r"INPUT knob6 [+-]\d+ \(routing\.monitorLevel",
    "nav.liveSampling": r"NAV sampling\.live ",
    "live.knobs": r"INPUT knob2 [+-]\d+ \(live\.length",
}
# Features whose action leaves no specific log line (the evidence above is only that the session ran past them).
WEAK = {"track.selectSelector", "track.selectShiftPad", "seq.erase", "seq.targets", "amp.setting", "settings.reset"}


def log_lines(path):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            i = line.find(TAG)
            if i < 0:
                if "REALTIME VIOLATION" in line:
                    out.append(("RAW", line.strip()))
                continue
            rest = line[i + len(TAG):].strip()
            rest = rest.split(" ", 1)[1] if " " in rest else ""   # the project's env prefix word
            sp = rest.find(" ")
            out.append((rest[:sp] if sp > 0 else rest, rest[sp + 1:] if sp > 0 else ""))
    return out


def wav_check(path, tail_s, check):
    with wave.open(path, "rb") as w:
        ch, width, rate, total = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        if width != 2:
            check(False, f"WAV: 16-bit expected, got {8 * width}-bit")
            return
        chunk = rate * 10
        peaks = []
        pos = 0
        while pos < total:
            n = min(chunk, total - pos)
            data = w.readframes(n)
            samples = struct.unpack(f"<{n * ch}h", data[:n * ch * 2])
            pk = 0
            for c in (0, 1):
                s = samples[c::ch]
                pk = max(pk, max(s), -min(s))
            peaks.append(pk)
            pos += n
        tail_frames = int(tail_s * rate)
        w.setpos(max(0, total - tail_frames))
        data = w.readframes(tail_frames)
        n = len(data) // (2 * ch)
        samples = struct.unpack(f"<{n * ch}h", data[:n * ch * 2])
        tail_pk = 0
        for c in (0, 1):
            s = samples[c::ch]
            if s:
                tail_pk = max(tail_pk, max(s), -min(s))

    def dbfs(v):
        return 20 * math.log10(v / 32768.0) if v > 0 else float("-inf")

    active = max(peaks[:-1] or [0])
    loud_windows = sum(1 for p in peaks if dbfs(p) > ACTIVE_MIN_DBFS)
    check(dbfs(active) > ACTIVE_MIN_DBFS,
          f"the set sounded: MAIN peak {dbfs(active):.1f} dBFS (> {ACTIVE_MIN_DBFS:g}); {loud_windows} of "
          f"{len(peaks)} 10-s windows above {ACTIVE_MIN_DBFS:g} dBFS ({total / rate:.0f} s of audio)")
    check(dbfs(tail_pk) <= TAIL_MAX_DBFS,
          f"no stuck notes: MAIN peak over the last {tail_s:g} s (after STOP, STOP) {dbfs(tail_pk):.1f} dBFS "
          f"(<= {TAIL_MAX_DBFS:g})")


def verify_cmd(a) -> int:
    failed = []

    def check(cond, text):
        print(("  OK   " if cond else "  FAIL ") + text)
        if not cond:
            failed.append(text)

    m = json.load(open(a.manifest))
    lines = log_lines(a.log)
    text = [f"{k} {v}" for k, v in lines]
    print(f"liveset: seed={m['seed']} minutes={m['minutes']:g} events={m['events']} routines={len(m['routines'])} "
          f"(cycle 1 = {m['cycle1_ms'] / 1000:.0f} s)")

    # exit code, FATAL, RT
    check(a.exit_code == 0, f"exit code {a.exit_code} (0; 134 = abort, e.g. a REALTIME VIOLATION)")
    fatal = [t for t in text if t.startswith("FATAL")]
    check(not fatal, f"no FATAL ({len(fatal)}{': ' + fatal[0] if fatal else ''})")
    rt = [t for t in text if "REALTIME VIOLATION" in t]
    check(not rt, f"zero RT violations (APP_RT_CHECKS: no REALTIME VIOLATION report{': ' + rt[0] if rt else ''})")
    check(any(k == "STOP" for k, _ in lines), "STOP reached (clean exit path)")

    # script
    sc = [t for t in text if t.startswith("SCRIPT ")]
    check(bool(sc) and sc[0].startswith("SCRIPT ok"), f"script parsed: {sc[0][:110] if sc else 'no SCRIPT line'}")
    bad, gaps = [], 0
    for line in open(a.script):
        line = line.split("#", 1)
        body, note = line[0].split(), (line[1] if len(line) > 1 else "")
        if len(body) >= 3 and body[1] == "cmd":
            c = " ".join(body[2:])
            if "GAP" in note and any(c.startswith(g) for g in GAP_CMDS):
                gaps += 1
            elif not any(c == x or c.startswith(x + " ") for x in ALLOWED_CMDS):
                bad.append(c)
    check(not bad, f"cmd only for checkpoints ({', '.join(ALLOWED_CMDS)}) and marked NFR-040 gaps: {gaps} GAP lines"
                   + (f"; not allowed: {bad[:3]}" if bad else ""))

    # AUDIO / LOAD
    audio = [v for k, v in lines if k == "AUDIO" and v.startswith("alsa_periods=")]
    loads = [v for k, v in lines if k == "LOAD"]
    bad_audio = [v for v in audio if " xruns=0 " not in v + " "]
    bad_load = [v for v in loads if " xruns=0 " not in v + " "]
    check(bool(audio) and not bad_audio, f"xruns 0 in every AUDIO line ({len(audio)} lines, {len(bad_audio)} not 0)")
    expect = int(m["quit_ms"] / 10000) - 2
    check(len(loads) >= expect and not bad_load,
          f"xruns 0 in every LOAD line ({len(loads)} lines, >= {expect} expected, {len(bad_load)} not 0)")
    rx = re.compile(r"avg=(\d+) p95=(\d+) max=(\d+) load=([\d.]+)% overruns=(\d+) periods=(\d+) xruns=(\d+) "
                    r"VmRSS=(\d+) kB")
    parsed = [tuple(float(x) for x in mm.groups()) for mm in (rx.search(v) for v in loads) if mm]
    if parsed:
        p95s = [p[1] for p in parsed]
        p95s_sorted = sorted(p95s)
        worst = max(p95s)
        check(worst <= LOAD_P95_MAX,
              f"LOAD p95 <= {LOAD_P95_MAX} cycles/block in every 10-20 s window: worst {worst:.0f}, median "
              f"{p95s_sorted[len(p95s) // 2]:.0f} ({100 * worst / LOAD_P95_MAX / 2:.2f} % of the block; desktop ns = "
              f"cycles, a relative gate)")
        print(f"  info LOAD max {max(p[2] for p in parsed):.0f} cycles (one block), avg {parsed[-1][0]:.0f}, "
              f"load {max(p[3] for p in parsed):.1f} % max; overruns {parsed[-1][4]:.0f} of {parsed[-1][5]:.0f} "
              f"blocks (reported, not gated)")
        # RSS: baseline at the first LOAD line from 20 s on (the kit is loaded), max of the last third
        times = [10.0 * (i + 1) for i in range(len(parsed))]
        base_i = next((i for i, t in enumerate(times) if t >= 20.0), 0)
        base = parsed[base_i][7] / 1024.0
        third = parsed[int(len(parsed) * 2 / 3):] or parsed[-1:]
        end_max = max(p[7] for p in third) / 1024.0
        bound = RSS_BASE_MB + RSS_PER_10MIN_MB * m["minutes"] / 10.0
        check(end_max - base <= bound,
              f"VmRSS growth {end_max - base:+.1f} MB (start {base:.1f} MB at ~{times[base_i]:.0f} s, max of the last "
              f"third {end_max:.1f} MB, final {parsed[-1][7] / 1024.0:.1f} MB; bound {bound:.0f} MB)")
    else:
        check(False, "no parsable LOAD line")

    # screens, saves
    for s in m["screens"]:
        n = sum(1 for t in text if t.startswith(f"NAV {s} ("))
        check(n > 0, f"NAV {s}: {n}x")
    # Saves: one SAVED line each. A save the store refused because the previous one was still running (SAVE_BUSY: the
    # script is time-based, the app is right to refuse) is accepted only when a later save of the session succeeded;
    # any other failure, or a session whose last save failed, fails the check.
    results = [(i, t) for i, t in enumerate(text) if re.match(r"SAVED \S+ (ok|FAILED)", t)]
    saved = [t for _, t in results if re.match(r"SAVED \S+ ok", t)]
    busy = [i for i, t in results if " FAILED " in t and SAVE_BUSY in t]
    other = [t for _, t in results if " FAILED " in t and SAVE_BUSY not in t]
    last_ok = max((i for i, t in results if re.match(r"SAVED \S+ ok", t)), default=-1)
    busy_ok = all(i < last_ok for i in busy)
    check(len(results) >= len(m["saves"]) and saved and not other and busy_ok,
          f"saves (PROJECT > SAVE on the panel): {len(saved)} SAVED ok, {len(busy)} refused while one ran "
          f"(each followed by a later ok: {'yes' if busy_ok else 'NO'}), {len(other)} failed, of "
          f"{len(m['saves'])} scripted" + (f"; {other[0]}" if other else ""))

    # lamps (lamp_log=1: LAMP lines on change, merged per 100 ms): the pad-mode keys light, the pads show the mode
    lamp_check(lines, check)
    rec = [int(mm.group(1)) for mm in (re.search(r"M2 STATE .* recorded=(\d+)", t) for t in text) if mm]
    check(bool(rec) and rec[-1] > 0, f"recording passes wrote hits (M2 STATE recorded={rec[-1] if rec else '?'})")

    # WAV
    if a.wav:
        try:
            wav_check(a.wav, TAIL_S, check)
        except (OSError, wave.Error, EOFError) as e:
            check(False, f"WAV {a.wav}: {e}")

    # probes: the M2 PATTERN line of the probe's scene / pattern after its `cmd state` checkpoint
    proven = {}
    state_at = [i for i, (k, v) in enumerate(lines) if k == "CMD" and v == "state"]
    for pr in m.get("probes", []):
        cells = None
        if pr["state"] < len(state_at):
            start = state_at[pr["state"]]
            end = state_at[pr["state"] + 1] if pr["state"] + 1 < len(state_at) else len(lines)
            for k, v in lines[start:end]:
                mm = re.match(rf"PATTERN scene={pr['scene']} pattern={pr['pattern']} .* cells=(\S*)", v)
                if k == "M2" and mm:
                    cells = mm.group(1).split(",")
        have = [c for c in pr["cells"] if cells and (c in cells or c + "L" in cells)]
        ok = len(have) == len(pr["cells"])
        check(ok, f"probe (state #{pr['state'] + 1}, scene {pr['scene']} pattern {pr['pattern']}): cells "
                  f"{','.join(pr['cells'])} {'present' if ok else 'missing: ' + str(sorted(set(pr['cells']) - set(have)))}"
                  f" ({' + '.join(pr['features'])})")
        for f in pr["features"]:
            proven[f] = proven.get(f, True) and ok
    # SCALE pad mode plays the selected track: a run of scale hits never changes it (the pads do not select, PAD-004)
    runs, last, changed = 0, None, 0
    for k, v in lines:
        mm = re.match(r"pad\d+ scale track=(\d+)", v) if k == "INPUT" else None
        if mm:
            changed += 1 if last is not None and mm.group(1) != last else 0
            runs += 1 if last is None else 0
            last = mm.group(1)
        elif k == "INPUT" and re.match(r"pad\d+ ", v):
            last = None  # another pad mode's hit ends the run
    check(runs > 0 and changed == 0, f"SCALE pad mode keeps the selected track ({runs} runs of scale hits, "
                                     f"{changed} track changes inside a run)")
    tr = [v for k, v in lines if k == "TRANSPORT"]
    proven["transport.hardStop"] = any(tr[i].startswith("stop ") and tr[i + 1].startswith("stop ")
                                       for i in range(len(tr) - 1))

    # coverage
    print("--- action coverage (features touched by the scripted panel / keyboard session)")
    missing = []
    for f in m["features"]:
        rx2 = EVIDENCE.get(f)
        if f in proven:
            hits, how = (1 if proven[f] else 0), "probe / sequence check"
        else:
            hits = sum(1 for t in text if rx2 and re.search(rx2, t)) if rx2 else 0
            how = "(scripted; no specific log line)" if f in WEAK else f"{hits} log lines"
        if not hits:
            missing.append(f)
        print(f"  {'ok' if hits else 'MISSING':7s} {f:24s} {how}")
    check(not missing, f"coverage: {len(m['features']) - len(missing)} of {len(m['features'])} features evidenced"
                       + (f"; missing {missing}" if missing else ""))
    print("--- controls used: " + " ".join(m["controls"]))
    print(f"=== liveset {'PASS' if not failed else 'FAIL'} ({len(failed)} failed)")
    return 1 if failed else 0


def lamp_check(lines, check):
    """Pad lamps (UI-152/156): replays the LAMP lines into a state map. While a pad-mode key is lit: TRACK =
    the selected pad on, the others dim; MUTE = muted pads dim; SCALE = root pads on, the other notes dim; SLICE = the
    slice pads lit; the mode keys one-hot. No flicker: in each MUTE interval the pad lamps change at most 16 x (the
    mute / solo hits + 2) times (a pad flipping between the shell's lamp and the mode's would exceed it)."""
    state = {}
    seen = {"track": 0, "mute": 0, "slice": 0, "scale": 0}
    shown = {"track": 0, "mute": 0, "slice": 0, "scale": 0}
    multi = 0
    mute_open, mute_changes, mute_hits, intervals = False, 0, 0, []
    for k, v in lines:
        if k == "INPUT" and mute_open and re.match(r"pad\d+ (mute|solo|mute-others) ", v):
            mute_hits += 1
        if k != "LAMP":
            continue
        changed_pads = 0
        for tok in v.split():
            name, _, val = tok.partition("=")
            rgb, _, lvl = val.partition("/")
            if name.startswith("pad.") and state.get(name, ("", 0))[1] != int(lvl or 0):
                changed_pads += 1
            state[name] = (rgb, int(lvl or 0))
        lit = {m: state.get(f"padmode.{m}", ("", 0))[1] >= 200 for m in seen}
        pads = [state.get(f"pad.{i}", ("", 0))[1] for i in range(1, 17)]
        on, dim = sum(1 for p in pads if p >= 200), sum(1 for p in pads if 0 < p < 200)
        multi += 1 if sum(lit.values()) > 1 else 0
        for m in seen:
            seen[m] += 1 if lit[m] else 0
        shown["track"] += 1 if lit["track"] and on >= 1 and dim >= 1 else 0
        shown["mute"] += 1 if lit["mute"] and dim >= 1 else 0
        shown["scale"] += 1 if lit["scale"] and on >= 1 and dim >= 1 else 0
        shown["slice"] += 1 if lit["slice"] and on + dim >= 1 else 0
        if lit["mute"] and not mute_open:
            mute_open, mute_changes, mute_hits = True, 0, 0
        elif mute_open and lit["mute"]:
            mute_changes += changed_pads
        elif mute_open and not lit["mute"]:
            mute_open = False
            intervals.append((mute_changes, mute_hits))
    # the interval closest to (or over) its bound
    mute_worst = max(intervals, key=lambda c: c[0] - 16 * (c[1] + 2)) if intervals else (0, 0)
    what = {"track": "the selected pad on, the others dim", "mute": "muted pads dim", "scale": "roots on, others dim",
            "slice": "the slice pads lit"}
    for m in seen:
        check(seen[m] > 0 and shown[m] > 0, f"LAMP padmode.{m} lit ({seen[m]} LAMP lines) with {what[m]} "
                                            f"({shown[m]} lines)")
    check(multi == 0, f"LAMP pad-mode keys one-hot ({multi} lines with two lit)")
    check(bool(intervals) and mute_worst[0] <= 16 * (mute_worst[1] + 2),
          f"LAMP no flicker in MUTE mode: {len(intervals)} MUTE intervals, worst {mute_worst[0]} pad lamp changes for "
          f"{mute_worst[1]} mute / solo hits (<= 16 x (hits + 2))")
    end_lit = [m for m in seen if state.get(f"padmode.{m}", ("", 0))[1] > 0]
    check(not end_lit, f"LAMP every pad-mode key off at the end (PLAY){': lit ' + str(end_lit) if end_lit else ''}")


# --- the NFR-040 audit ------------------------------------------------------------------------------------------------
# (feature, action, panel / keyboard path, status, where). Status: OK = panel and keyboard; KEY = keyboard only (an
# extension control, UI-102); GAP = no panel or keyboard path (touch / mouse only, or nothing).
AUDIT = [
    ("PERF", "play pads (PLAY mode)", "pad.1-16 | Z X C V / A S D F / Q W E R / 1 2 3 4", "OK", "PadController"),
    ("PERF", "TRACK / MUTE / SLICE / SCALE pad modes", "padmode.* | F5-F8 (lit key ends the mode)", "OK",
     "PadController"),
    ("PERF", "mute / unmute a track", "MUTE mode + pad.n | F6 + pad key", "OK", "PadController"),
    ("PERF", "solo (mute all others, again = unmute all)", "MUTE mode + SHIFT + pad.n | F6, Shift + pad key", "OK",
     "PadController (PAD-006)"),
    ("PERF", "select a track", "sel on HOME, TRACK mode + pad, SHIFT + pad | , . / F5 + pad / Shift + pad", "OK",
     "HomeScreen, PadController"),
    ("SEQ", "step entry (toggle a trigger)", "step.1-16 short press | 5 6 7 8 9 0 - = T Y U I O P [ ]", "OK",
     "StepController"),
    ("SEQ", "step lock", "hold step.n + turn enc.n | hold step key + Ctrl+n, Up/Down", "OK", "Shell::turnEncoder"),
    ("SEQ", "shown bar", "bar.1-4 | F1-F4", "OK", "Document::handle"),
    ("SEQ", "record a pass", "rec + play | G + Space", "OK", "TransportController, StepController"),
    ("SEQ", "erase while playing", "ext.erase held | J (keyboard only)", "KEY", "StepController (SEQ-046)"),
    ("SEQ", "clear a step (ext.clear)", "hold step.n + ext.clear | step key + Delete (UI-120)", "KEY",
     "PanelKeys -> StepController::clearHeldStep (SEQ-012)"),
    ("TRANSPORT", "play / pause / stop / hard stop", "play, stop | Space, B", "OK", "TransportController"),
    ("TRANSPORT", "tempo (tap)", "ext.tap | N (keyboard only)", "KEY", "TransportController (SYN-003)"),
    ("TRANSPORT", "tempo (BPM value), swing, quantize, metronome",
     "SHIFT + ext.tap opens BPM/QUANTIZE (our choice); sel = value, push = next field, SHIFT + push = metronome | "
     "Shift+N, , . Return, Shift+Return", "KEY", "Shell (SHIFT + ext.tap), BpmQuantizePanel"),
    ("TRANSPORT", "nudge", "ext.nudgeDown / ext.nudgeUp held | ; and ' (our choice: UI-120 lists no key)", "KEY",
     "KeyMap.cpp, TransportController (SYN-005), BpmQuantizePanel"),
    ("LFO", "open LFO", "HOME sel.push -> TRACK MENU, sel to LFO, sel.push | Return, . , Return", "OK",
     "HomeScreen, TrackMenuScreen"),
    ("LFO", "SPEED / DEPTH / PHASE / DESTINATION / SHAPE", "enc.1-5 | Ctrl+1-5, Up/Down", "OK", "LFO strip"),
    ("LFO", "DESTINATION / SHAPE lists, SETTING, SYNC", "sel to the rail item + sel.push | , . Return", "OK",
     "Shell rail (LfoGraph leaves the rotary to the rail)"),
    ("LFO", "LFO step locks", "hold step.n + enc.1-3 | step key + Up/Down", "OK", "Shell::turnEncoder"),
    ("PERF", "ratchets (RETRIGGER CNT / SPD)", "TRACK MENU -> SEQUENCE, step keys = targets, enc.2/3", "OK",
     "SequenceScreen"),
    ("PERF", "transpose", "SEQUENCE enc.6", "OK", "SequenceScreen"),
    ("PERF", "step shift (SHIFT, SHIFT AREA)", "SEQUENCE enc.4 / enc.5", "OK", "SequenceScreen"),
    ("PERF", "SEQUENCE TRIGGER TYPE / BAR / CLEAR / QUANTIZE", "SEQUENCE sel + sel.push", "OK", "SequenceScreen"),
    ("SLICE", "play slices", "padmode.slice + pads | F7 + pad keys", "OK", "Document::slicePad"),
    ("SLICE", "open SET SLICE", "SHIFT + lit padmode.slice | Shift + F7 (UI-403)", "OK", "Document::slicePad"),
    ("SLICE", "SLICE No. / POSITION / START / END / AREA", "SET SLICE enc.1-5", "OK", "SET SLICE strip"),
    ("SLICE", "ADD SLICE / DELETE SLICE / GRID SLICE (slice count)",
     "SET SLICE rail: holds the rotary on entry, sel + sel.push; past GRID SLICE the rotary zooms; SHIFT + sel.push "
     "toggles rail / waveform (our choice) | , . Return", "OK", "Shell::toggleRailFocus, SetSliceBody::railFocusEnter"),
    ("SLICE", "open SET SLICE / SET SCALE from PLAYBACK; REVERSE, GRID SNAP",
     "PLAYBACK: SHIFT + sel.push gives the rail the focus (our choice), sel + sel.push; past GRID SNAP the rotary zooms again "
     "| Shift+Return, , . Return", "OK", "Shell::toggleRailFocus, PlaybackBody::railFocusEnter"),
    ("SCALE", "open SET SCALE", "SHIFT + lit padmode.scale (UI-403) | Shift + F8; or PLAYBACK rail SET SCALE", "OK",
     "PanelKeys (SHIFT + lit SCALE)"),
    ("SCALE", "SCALE list / AREA / TIME STRETCH / KEY SHIFT (once open)", "SET SCALE rail + sel.push, enc.1-3", "OK",
     "SetScaleScreen"),
    ("AMP", "open AMP ENVELOPE", "TRACK MENU sel + sel.push", "OK", "TrackMenuScreen"),
    ("AMP", "VELOCITY / ATTACK / HOLD / RELEASE / TRIGGER MODE", "enc.1-5", "OK", "AMP ENVELOPE strip"),
    ("AMP", "SETTING", "sel.push (the body opens SETTING)", "OK", "AmpEnvelopeScreen"),
    ("AMP", "SYNC toggle (rail)", "SHIFT + sel.push = the rail (SETTING, our choice), sel to SYNC, sel.push; past SYNC the "
     "handles again | Shift+Return, . Return", "OK", "Shell::toggleRailFocus, AmpEnvelopeBody::railFocusEnter"),
    ("RIB", "strip PITCH / REPEAT / USER1 / USER2", "strip + strip.mode (F11); the strip has no keyboard map (UI-124)",
     "OK", "StripSource (panel strip; on the desktop Ctrl + wheel)"),
    ("RIB", "strip HOLD", "strip.hold | F12", "OK", "StripSource"),
    ("RIB", "open TOUCH STRIP SETTING USER1 / USER2", "SHIFT + HOME -> UTILITY, sel + sel.push | Shift+Home, Return",
     "OK", "Shell (SHIFT + HOME), ui/screens/utility (SettingsList)"),
    ("RIB", "assign / range / delete a USER item", "sel + sel.push (SELECT PARAMETER), enc.1-2, rail DELETE", "OK",
     "TouchStripScreen"),
    ("RIB", "PITCH range", "UTILITY row TOUCH STRIP SETTING (PITCH RANGE): sel + sel.push, sel, sel.push", "OK",
     "ui/screens/utility (SettingsList)"),
    ("EDIT", "copy / paste steps", "hold ext.copy + step keys, hold ext.paste + step | Cmd+C / Cmd+V held", "KEY",
     "StepController (EDT-005)"),
    ("EDIT", "open SCENE MANAGER", "SHIFT + scene (UI-403; again = close) | Shift + F9", "OK",
     "PanelKeys (SHIFT + SCENE)"),
    ("EDIT", "copy / paste / delete pattern, scene, track (SCENE MANAGER)",
     "sel / sel.push to the key, ext.copy / ext.paste / ext.clear (SHIFT + paste = swap) | Cmd+C, Cmd+V, Delete", "KEY",
     "SceneManagerBody::handle (UI-102)"),
    ("EDIT", "undo / redo", "ext.undo / ext.redo | Cmd+Z / Shift+Cmd+Z (EDT-007)", "KEY",
     "PanelKeys (refused while a recording pass is open)"),
    ("SCENE", "pattern switch", "PATTERN + step key | F10 + step key", "OK", "ScenePatternKeys"),
    ("SCENE", "scene switch", "SCENE + scene key + pattern key | F9 + step keys", "OK", "ScenePatternKeys"),
    ("SCENE", "pattern chain", "PATTERN mode, hold the current pattern's key + others", "OK", "ScenePatternKeys"),
    ("SCENE", "pattern length", "SHIFT + PATTERN, BAR / step key | Shift + F10, F1-F4", "OK", "ScenePatternKeys"),
    ("NAV", "HOME / BACK / UTILITY", "home, back, SHIFT + home | Home or `, Esc, Shift + Home", "OK", "Shell"),
    # UTILITY and its sub-pages (07-ui-design/screens/utility.md), all on the rotary
    ("SET", "UTILITY rows (focus, SHIFT + turn = next section)", "sel, Shift + sel | , . (Shift)", "OK",
     "ui/screens/utility/SettingsList"),
    ("SET", "edit a value (push, turn, push; BACK cancels)", "sel.push, sel, sel.push / back | Return , . Return / Esc",
     "OK", "SettingsList + EditCapture"),
    ("SET", "a list of more than 8 values (ListPopup)", "sel.push, sel, sel.push | Return , . Return", "OK",
     "SettingsList -> ListPopup"),
    ("SET", "open a page (›), BACK to the same row", "sel.push, back | Return, Esc", "OK", "SettingsList (UID-879)"),
    ("SET", "CHANNEL MAPPING (MIDI IN / OUT), RESET (confirmation)", "UTILITY rows + sel.push", "OK",
     "SettingsList (utility.channelMapIn / Out)"),
    ("SET", "MIDI PORTS, AUDIO DEVICE, OUTPUT CHANNEL MAP, LIBRARY LOCATIONS", "UTILITY rows + sel.push", "OK",
     "SettingsList (utility.midiPorts / audioDevice / outputMap / library)"),
    ("SET", "LIBRARY LOCATIONS: ADD LOCATION (folder path) / remove", "sel.push -> text dialog (keyboard / on-screen "
     "keyboard), Message", "OK", "SettingsList -> Dialog"),
    ("SET", "CONTROLLERS, KEYBOARD MAP pages", "UTILITY rows + sel.push (rows disabled while the pages are not "
     "registered)", "OK", "SettingsList"),
    ("SET", "Version No., DISK SPACE", "sel (info rows are focusable, push does nothing)", "OK", "SettingsList"),
    ("NAV", "PROJECT (save, open, new, render)", "ext.project, then sel + sel.push on the rows | Cmd+4, , . Return "
     "(Cmd+S/O/N are macOS menu-bar commands: none on linux)", "KEY", "Shell::jumpKey, ProjectList"),
    ("NAV", "MIXER, ARRANGER, TRACK SETTING", "ext.mixer, SHIFT + ext.mixer, SHIFT + ext.utility (our choice) | Cmd+1, "
     "Shift+Cmd+1, Shift+Cmd+3", "KEY", "Shell::jumpKey"),
    ("NAV", "UTILITY", "SHIFT + home, ext.utility | Shift+Home, Cmd+3", "OK", "Shell"),
    # the sampling, MIDI and song screens (sampling.md, midi-track.md, mixer.md OUTPUT SETTING, arranger.md, project.md
    # RENDER).
    ("SAMPLING", "open SAMPLING/EDIT, LIVE SAMPLING", "TRACK MENU sel to the header action + sel.push; SHIFT + rec = "
     "LIVE SAMPLING | Shift+G", "OK", "TrackMenuScreen header actions; PanelKeys (SHIFT + REC, W3)"),
    ("SAMPLING", "REC / stop, LIVE arm / disarm", "rec | G (the screen's REC while it is open)", "OK",
     "SamplingScreen / LiveSamplingScreen handle(Rec); TransportController leaves REC to them"),
    ("SAMPLING", "SOFT LIMITER, EDIT list (BAR, BPM, SAVE SELECTION, NORMALIZE, FADE IN / OUT), SAVE CHANGES",
     "sel walks the rail (the editor keeps no rotary) + sel.push; list rows sel + sel.push", "OK", "SamplingScreen"),
    ("SAMPLING", "SOURCE, THRESHOLD, REC LENGTH, SELECTION START / END, MONITOR", "enc.1-6 (SHIFT = 1 frame)", "OK",
     "SamplingScreen StripOwner"),
    ("SAMPLING", "SAVE SELECTION name, CANCEL / SAVE / SAVE & REPLACE; the unsaved-take question",
     "typing + Return / Esc; Message sel + sel.push", "OK", "Dialog, Message"),
    ("SAMPLING", "LIVE TRACK / LENGTH / LOOP / NUDGE / SAVE TO / MONITOR, UNLOAD, SAVE", "enc.1-6; rail sel + sel.push",
     "OK", "LiveSamplingScreen"),
    ("MIDI", "open MIDI NOTE / CC1 / CC2, SELECT CH, BANK SELECT, PROG CHANGE", "TRACK MENU (MIDI) sel + sel.push",
     "OK", "TrackMenuScreen, MidiTrackScreen popups"),
    ("MIDI", "NOTE / VELOCITY / LENGTH / NOTE2-4; CC values; CC ACTIVE; CC NUMBER",
     "enc.1-6; sel = card, sel.push = ACTIVE, SHIFT + sel.push = CC NUMBER list, SHIFT + enc = CC number", "OK",
     "MidiTrackScreen"),
    ("MIDI", "chord note by SHIFT + tap on a key", "NOTE2-4 on enc.4-6 (offsets)", "OK", "MidiTrackScreen"),
    ("OUTPUT", "open OUTPUT SETTING", "ext.mixer, then sel back from strip 1 to the rail OUTPUT SETTING, sel.push",
     "OK", "MixerScreen rail"),
    ("OUTPUT", "ASSIGN MODE, HP CUE, OUT ASSIGN, THRU BYPASS", "sel to the rail + sel.push (lists); enc.3, enc.5",
     "OK", "OutputScreen"),
    ("OUTPUT", "ANALOG FILTER SETTING rows (afs.*), PROCESSOR EXTERNAL page", "MIXER rail sel + sel.push; afs.* panel knobs",
     "OK", "MixerScreen, Document::afsControl"),
    ("ARR", "open ARRANGER", "SHIFT + ext.mixer | Shift+Cmd+1 (jump key)", "KEY", "Shell::jumpKey"),
    ("RENDER", "open RENDER AUDIO", "ext.project, sel to RENDER AUDIO, sel.push | Cmd+4", "KEY", "Shell::jumpKey, ProjectList"),
    ("ARR", "rows: insert, edit, move, COPY / PASTE / DELETE, clear, undo / redo",
     "sel.push, enc.1-4, SHIFT + sel, rail via sel | Cmd+C / Cmd+V / Delete / Shift+Delete / Cmd+Z (on ARRANGER)",
     "OK", "ArrangerScreen"),
    ("ARR", "ARRANGEMENT on / off, start from the focused row", "sel up from row 1 to the rail + sel.push; SHIFT + sel.push "
     "with ARRANGEMENT on (control-surface.md SHIFT table, M p.26); off: SHIFT + sel.push toggles the rail focus",
     "OK", "ArrangerScreen (RailOwner::ownsShiftPush), Shell"),
    ("RENDER", "targets, RENDER MODE, REMAINDER, DESTINATION, REPEATS / BIT DEPTH / NORMALIZE, RENDER, CANCEL",
     "sel + sel.push; enc.1-3; Progress sel.push = CANCEL", "OK", "RenderScreen"),
    ("NAV", "BROWSE (SHIFT + sel.push on HOME, ext.browse)",
     "SHIFT + sel.push on HOME (UI-403), ext.browse, TRACK MENU sel + sel.push | Shift+Return, Cmd+2", "OK",
     "HomeScreen (TrackGrid), Shell::jumpKey"),
    ("PERF", "pad-mode lamps (mode keys, dimmed mutes, SCALE roots, SLICE pads, TRACK selection)",
     "lamps follow the pad mode (UI-152/156)", "OK", "PadController::paintLamps -> PadLamps -> the lamp frame"),
    # controllers (UI-130…136, UI-120, UI-330): the controller pages are UTILITY sub-pages, driven like every list.
    ("CTRL", "controller role / profile / pad response / trim",
     "UTILITY > CONTROLLERS: sel to the row, sel.push (edit), sel turns, sel.push | , . Return", "OK",
     "ui/screens/controllers/ControllerPages.cpp"),
    ("CTRL", "MIDI learn (user profile)", "CONTROLLERS > MIDI LEARN: move the device's control, sel + sel.push "
     "(LOGICAL CONTROL list) | , . Return", "OK", "ControllerHub::assignLearned"),
    ("CTRL", "remap a key (KEYBOARD MAP)", "sel + sel.push, then the new key (Esc cancels); SHIFT + sel.push or "
     "ext.clear = default | , . Return, Delete", "OK", "KeyRemap (platform/host/input/KeyMap.cpp hooks)"),
    ("CTRL", "show / hide the virtual panel", "Ctrl + \\ (keyboard); the drawer bar / HIDE by pointer", "KEY",
     "ui/screens/virtualpanel/PanelView.cpp (PanelHost)"),
]


def coverage_cmd(a) -> int:
    print("NFR-040 audit (performance + the live set's actions): status OK = panel and keyboard, KEY = keyboard only "
          "(extension control), GAP = touch / mouse only or unreachable")
    w = max(len(r[1]) for r in AUDIT)
    for area, action, path, status, where in AUDIT:
        print(f"  {status:4s} {area:9s} {action:{w}s}  {path}")
        if status.startswith("GAP") or status.endswith("*"):
            print(f"       {'':9s} {'':{w}s}  ({where})")
    gaps = [r for r in AUDIT if r[3] == "GAP"]
    print(f"--- {len(AUDIT)} actions, {sum(1 for r in AUDIT if r[3] == 'OK')} OK, "
          f"{sum(1 for r in AUDIT if r[3] == 'KEY')} keyboard only, {len(gaps)} GAP (NFR-040 findings)")
    print("--- the UI-120 key of each panel control the live set uses:")
    for c in sorted(KEYS):
        if c.startswith(("pad.", "step.")) and not c.endswith((".1", ".16")):
            continue
        print(f"  {c:14s} {'panel' if panel_has(c) else 'keyboard only':13s} {KEYS[c]}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("script")
    s.add_argument("--out", required=True)
    s.add_argument("--manifest", required=True)
    s.add_argument("--minutes", type=float, default=30.0)
    s.add_argument("--seed", type=int, default=1604)
    s.add_argument("--device", action="store_true", help="for the reference hardware (no KEYBOARD MAP row)")
    v = sub.add_parser("verify")
    v.add_argument("--log", required=True)
    v.add_argument("--manifest", required=True)
    v.add_argument("--script", required=True)
    v.add_argument("--wav")
    v.add_argument("--exit-code", type=int, default=0)
    sub.add_parser("coverage")
    a = ap.parse_args()
    global DEVICE
    DEVICE = bool(getattr(a, "device", False))
    if a.cmd == "script" and a.minutes < 2:
        print("--minutes: at least 2 (cycle 1 covers every feature in the first ~100 s)", file=sys.stderr)
        return 2
    return {"script": script_cmd, "verify": verify_cmd, "coverage": coverage_cmd}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
