#!/usr/bin/env python3
"""Build a synthetic reference project (and its disk tree) from a JSON description — the only kind of project file
this repository's tests, benchmarks and smoke checks ever read. Written from the documented layout (research
docs/project-format.md, CC BY 4.0; spec 04-file-formats/sp16-interop.md); never from device data.

  tools/prj-dump/prj-synth.py DESC.json OUT_DIR [--name NAME] [--print]
      writes OUT_DIR/PIONEER DJ SAMPLER/Projects/<name>.prj, System/data.rsm (a byte copy, research storage.md §7),
      Samples/<file>.wav + .wav.dat for every sample the description lists; prints the .prj path
  tools/prj-dump/prj-synth.py --default OUT.prj
      a default project only (no samples)

The schema the generator fills is the documented tree (ProjectData → SceneData × 16 → TrackData × 16 → Playback…,
PatternData, SendFx/MasterFx, RoutingData; ArrangementData; RibbonData; MidiSettingData) with the documented field
names; nodes whose field names the research does not list use the spec data model's names (the importer's [S]
assumption) — so these fixtures test the codec and the mapping machinery, not the guesses. Real files test those.

Two schemas ("schema"): "observed" — the property names, types and encodings a real 1.6x project uses (learned
from a real file, no values copied: observed names such as attackNorm / bVelocityEffective / bType1 / param3CcNo /
assignableOutCh / topCent, "FxType_" effect names, "MT" stretch, "Full" LFO trigger, packed hi16/lo16 ints for the
quantise units and retrigger speed, -1 "none" sentinels, int MIDI flags, $Ext0/%Smp sample URLs); "spec" (default) —
the research page's and the spec data model's names (the importer's [D] / [S] names).

Description (all keys optional; scene / track / pattern / step numbers 1-based):
  {"name": "Basic", "version": [1, 60], "schema": "observed",
   "project": {"projectBpm": 124.0, ...},                         prop overrides on ProjectData (typed JSON values)
   "omitScenes": [16],                                            scenes left out of the file
   "defaultPatterns": 4,                                          PatternData0..N-1 per scene (default 4)
   "patternNames": "local" | "global" | "none",                   PatternData<k> | <scene*16+k> | no suffix
   "scenes": {"1": {"props": {"name": "A", "bpm": 120.0},
                    "tracks": {"1": {"TrackData": {...}, "PlaybackData": {...}, "InsertFx0": {...}, ...}},
                    "patterns": {"6": {"Length": 16, "mutes": [2], "solos": [], "steps": [STEP, ...]}},
                    "dropPatterns": [2], "SendFx": {...}, "MasterFx": {...}, "RoutingData": {...}}},
   STEP = {"step": 1, "track": 1, "trigger": 100, "value2": 100, "type": 15,
           "locks": {"<slot 0-34>": {"raw": n} | {"k127": k} | {"norm": 0..1} | {"frames": n} | {"word": w},
                     ... + optional "vct": 0-3, "idx": n per lock}}
   "arrangement": {"length": 3, "records": {"0": [1, 2]}},        non-default Sections records (0-based index)
   "fill": {"seed": 1, "cells": 0.3, "locks": 4},                 random triggers (share of cells) with up to N
                                                                  random raw / k/127 / FX locks in every pattern
                                                                  (a full-size project for the benchmark)
   "extraProps": [{"path": "SceneData2/TrackData4", "name": "x", "value": {"int": 1}}],
   "extraNodes": [{"path": "", "node": {"type": "Mystery", "props": {"a": 1}}}],
   "removeProps": [{"path": "SceneData0/TrackData0/PlaybackData", "name": "bBypass"}],
   "samples": {"kick.wav": {"freq": 60, "ms": 120, "gain": 0.8, "dat": {"bar": 0.25, "bpm": 0, "sampleNum": 1}}}}
Typed values: plain JSON (int → int, float → double, …) or {"int64": n}, {"binary": "<base64>"}, {"void": null}.
Standard library only.
"""
import argparse
import json
import math
import os
import pathlib
import random
import struct
import sys
import wave

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sp16prj as P  # noqa: E402

ROOT_FOLDER = "PIONEER DJ SAMPLER"  # research storage.md §1


SCHEMA = "spec"


def packed(hi, lo):
    return (hi << 16) | lo


def fx(effect="Off"):
    params = ("params", P.A(P.A([P.S("None"), P.D(0.0)]) for _ in range(6)))
    if SCHEMA == "observed":
        return P.Node("InsertFx", [("effectType", P.S(effect)), params, ("bSync", P.B(False)), ("bBypass", P.B(False)),
                                   ("bType1", P.S("FilterTypeOff")), ("bType2", P.S("FilterTypeOff"))])
    return P.Node("InsertFx", [("effectType", P.S(effect)), params,
                               ("sync", P.B(False)), ("bypass", P.B(False)), ("type1", P.I(0)), ("type2", P.I(0))])


def track_observed(t):
    tr = P.Node(f"TrackData{t}", [
        ("trackMode", P.S("Sample")), ("triggerMode", P.S("OneShot")), ("chokeGroup", P.I(-1)),
        ("volume", P.I(100)), ("pan", P.I(0)), ("soloSetting", P.B(False)), ("muteSetting", P.B(False)),
        ("colourIndex", P.I(t)), ("instrument", P.S("Off"))])
    i01 = lambda v: P.I(1 if v else 0)  # noqa: E731 - MIDI flags are ints 0 / 1 in this schema
    cc1 = [("MidiBypassFlg", i01(False)), ("pitchBendValue", P.I(0)), ("pitchBendActive", i01(False)),
           ("afterTouchValue", P.I(0)), ("afterTouchActive", i01(False))]
    for k, cc in zip(range(3, 7), (7, 1, 2, 10)):
        cc1 += [(f"param{k}CcNo", P.I(cc)), (f"param{k}Value", P.I(0)), (f"param{k}Active", i01(False))]
    cc2 = [("MidiBypassFlg", i01(False))]
    for k in range(1, 7):
        cc2 += [(f"param{k}CcNo", P.I(70 + k)), (f"param{k}Value", P.I(0)), (f"param{k}Active", i01(False))]
    tr.children = [
        P.Node("PlaybackData", [("startSample", P.I(0)), ("loopStartSample", P.I(0)), ("lengthSample", P.I(0)),
                                ("bLoop", P.B(False)), ("bReverse", P.B(False)), ("pitchCent", P.I(0)),
                                ("stretchMode", P.S("Off")), ("audioSourceUrl", P.S("")), ("bBypass", P.B(False))]),
        P.Node("AmpEnvelopeData", [("attackNorm", P.D(0.0)), ("holdNorm", P.D(1.0)), ("releaseNorm", P.D(0.0)),
                                   ("volumeIndex", P.I(127)), ("bSync", P.B(False)), ("bVelocityEffective", P.B(False)),
                                   ("bRetriggerMode", P.B(True)), ("attackCurveType", P.S("Linear")),
                                   ("releaseCurveType", P.S("Linear")), ("bBypass", P.B(False))]),
        P.Node("SequencerData", [("retriggerCount", P.I(1)), ("retriggerSpeed", P.I(packed(1, 4))), ("offset", P.I(0)),
                                 ("stepShiftArea", P.I(-1)), ("transpose", P.I(0))]),
        P.Node("SliceData", [("grids", P.A([])), ("nGrids", P.I(16)), ("windowOffset", P.I(0))]),
        P.Node("ScaleSetting", [("scaleType", P.S("Chromatic")), ("keyShift", P.I(0)), ("windowOffset", P.I(0))]),
        P.Node("ScaleSettingMidi", [("scaleTypeMidi", P.S("Chromatic")), ("scalePosition", P.I(60)),
                                    ("rootNote", P.I(60))]),
        rename(fx(), "InsertFx0"), rename(fx(), "InsertFx1"),
        P.Node("SendData", [("sendChannel", P.I(0)), ("sendAmount", P.I(0))]),
        P.Node("LfoData", [("speed", P.D(21 / 127)), ("depth", P.I(64)), ("phaseOffset", P.I(0)),
                           ("destination", P.I(0)), ("type", P.S("Sine")), ("triggerMode", P.S("Trigger")),
                           ("triggerOn", P.S("Full")), ("sync", P.B(False)), ("randStartNum", P.I(0)),
                           ("bypass", P.B(False))]),
        P.Node("MidiTrackData", [], [
            P.Node("MidiNoteData", [("note1", P.I(60)), ("note2", P.I(0)), ("note3", P.I(0)), ("note4", P.I(0)),
                                    ("velocity", P.I(100)), ("length", P.I(4))]),
            P.Node("MidiCc1Data", cc1),
            P.Node("MidiCc2Data", cc2),
            P.Node("MidiOtherData", [("bankselmsb", P.I(0)), ("banksellsb", P.I(0)), ("programchange", P.I(0))]),
            P.Node("MidiModeData", [("midimode", P.I(-1))]),
        ]),
    ]
    return tr


def track(t):
    if SCHEMA == "observed":
        return track_observed(t)
    tr = P.Node(f"TrackData{t}", [
        ("trackMode", P.S("Sample")), ("triggerMode", P.S("OneShot")), ("chokeGroup", P.I(0)),
        ("volume", P.I(100)), ("pan", P.I(0)), ("soloSetting", P.B(False)), ("muteSetting", P.B(False)),
        ("colourIndex", P.I(t)), ("instrument", P.S("Off"))])
    tr.children = [
        P.Node("PlaybackData", [("startSample", P.I(0)), ("loopStartSample", P.I(0)), ("lengthSample", P.I(0)),
                                ("bLoop", P.B(False)), ("bReverse", P.B(False)), ("pitchCent", P.I(0)),
                                ("stretchMode", P.S("Off")), ("audioSourceUrl", P.S("")), ("bBypass", P.B(False))]),
        P.Node("AmpEnvelopeData", [("attack", P.D(0.0)), ("hold", P.D(1.0)), ("release", P.D(0.0)),
                                   ("volumeIndex", P.I(100)), ("sync", P.B(False)), ("velocitySensitive", P.B(True)),
                                   ("retrigger", P.B(False)), ("attackCurve", P.S("Linear")),
                                   ("releaseCurve", P.S("Linear")), ("bypass", P.B(False))]),
        P.Node("SequencerData", [("retriggerCount", P.I(1)), ("retriggerSpeed", P.I(2)), ("offset", P.I(0)),
                                 ("stepShiftArea", P.I(1)), ("transpose", P.I(0))]),
        P.Node("SliceData", [("grids", P.A([])), ("nGrids", P.I(16)), ("windowOffset", P.I(0))]),
        P.Node("ScaleSetting", [("scaleType", P.I(0)), ("keyShift", P.I(0)), ("windowOffset", P.I(0))]),
        P.Node("ScaleSettingMidi", [("scaleTypeMidi", P.I(0)), ("scalePosition", P.I(0)), ("rootNote", P.I(60))]),
        rename(fx(), "InsertFx0"), rename(fx(), "InsertFx1"),
        P.Node("SendData", [("sendChannel", P.I(0)), ("sendAmount", P.I(0))]),
        P.Node("LfoData", [("speed", P.D(21.0)), ("depth", P.I(0)), ("phaseOffset", P.I(0)), ("destination", P.I(0)),
                           ("type", P.S("Sine")), ("triggerMode", P.S("Trigger")), ("triggerOn", P.S("Pad")),
                           ("sync", P.B(False)), ("randStartNum", P.I(0)), ("bypass", P.B(False))]),
        P.Node("MidiTrackData", [], [
            P.Node("MidiNoteData", [("note1", P.I(60)), ("note2", P.I(-1)), ("note3", P.I(-1)), ("note4", P.I(-1)),
                                    ("velocity", P.I(100)), ("length", P.D(1.0)), ("MidiBypassFlg", P.B(False))]),
            # Field names not documented: the importer keeps and reports these (they test that path).
            P.Node("MidiCc1Data", [("pitchBendValue", P.I(0)), ("bPitchBend", P.B(False)),
                                   ("MidiBypassFlg", P.B(False))]),
            P.Node("MidiCc2Data", [("ccNo1", P.I(71)), ("ccValue1", P.I(0)), ("MidiBypassFlg", P.B(False))]),
            P.Node("MidiOtherData", [("bankMsb", P.I(-1)), ("bankLsb", P.I(-1)), ("program", P.I(-1))]),
            P.Node("MidiModeData", [("midimode", P.I(0))]),
        ]),
    ]
    return tr


def rename(n, t):
    n.type = t
    return n


def pattern(name, length=16, records=None, mutes=(), solos=()):
    return P.Node(name, [("Steps", P.BIN(P.steps_blob(records or {}))), ("Length", P.I(length)),
                         ("TrackMutes", P.A(P.B(t + 1 in mutes) for t in range(16))),
                         ("TrackSolos", P.A(P.B(t + 1 in solos) for t in range(16)))])


def pattern_name(style, scene, k):
    if style == "global":
        return f"PatternData{scene * 16 + k}"
    if style == "none":
        return "PatternData"
    return f"PatternData{k}"


def lock_words(locks, slot):
    """[(slot, word)] for one lock description (frames split into the high-part slot 16/17/18 for slots 1-3)."""
    vct, idx = int(locks.get("vct", 0)), int(locks.get("idx", slot))
    if "word" in locks:
        return [(slot, int(locks["word"]) & 0xFFFFFFFF)]
    if "raw" in locks:
        return [(slot, P.pack_word(int(locks["raw"]), idx, vct))]
    if "k127" in locks:
        return [(slot, P.pack_word(int(locks["k127"]) * P.K127, idx, vct))]
    if "norm" in locks:
        return [(slot, P.pack_word(int(round(float(locks["norm"]) * P.VALUE_FULL)), idx, vct))]
    if "frames" in locks:
        f = int(locks["frames"])
        out = [(slot, P.pack_word(f % P.FRAME_BASE, idx, vct))]
        if f >= P.FRAME_BASE:
            out.append((slot + 15, P.pack_word(f // P.FRAME_BASE, slot + 15)))
        return out
    raise ValueError(f"lock {locks!r}")


def step_records(steps):
    recs = {}
    for st in steps:
        r = P.empty_record()
        r[0] = int(st.get("trigger", -127))
        r[1] = int(st.get("value2", -126))
        r[2] = int(st.get("type", 15))
        for slot, lk in (st.get("locks") or {}).items():
            for s, w in lock_words(lk, int(slot)):
                r[3 + s] = w
        recs[(int(st["step"]) - 1, int(st["track"]) - 1)] = r
    return recs


def fill_records(fill, salt):
    rnd = random.Random(int(fill.get("seed", 1)) * 1000 + salt)
    recs = {}
    for st in range(P.STEPS):
        for t in range(P.TRACKS):
            if rnd.random() >= float(fill.get("cells", 0.3)):
                continue
            r = P.empty_record()
            r[0], r[1], r[2] = 100, rnd.randint(1, 127), rnd.choice([15, 15, 15, 11, 8])
            for _ in range(rnd.randint(0, int(fill.get("locks", 4)))):
                slot = rnd.choice([0, 4, 5, 6, 9, 11, 12, 13, 14, 19, 20, 23, 24, 25, 29, 30])
                if slot in (5, 6):
                    value = rnd.randint(0, 127) * P.K127
                elif slot >= 23:
                    value = rnd.randint(0, P.VALUE_FULL)
                elif slot == 4:
                    value = rnd.randint(-2400, 2400)
                else:
                    value = rnd.randint(0, 127)
                r[3 + slot] = P.pack_word(value, slot)
            recs[(st, t)] = r
    return recs


def apply_props(node, props):
    for k, v in (props or {}).items():
        node.set(k, v)


def find(root, path):
    n = root
    for part in [p for p in path.split("/") if p]:
        c = n.child(part)
        if c is None:
            raise KeyError(f"no node {part} in {n.type} (path {path})")
        n = c
    return n


def node_from(desc):
    n = P.Node(desc["type"])
    apply_props(n, desc.get("props"))
    n.children = [node_from(c) for c in desc.get("children", [])]
    return n


def build(desc, name):
    global SCHEMA
    SCHEMA = desc.get("schema", "spec")
    obs = SCHEMA == "observed"
    style = desc.get("patternNames", "local")
    proj = P.Node("ProjectData", [
        ("masterVolume", P.I(100)), ("headphoneVolume", P.I(100)), ("sendTrackVolume", P.I(100)),
        ("monitorVolume", P.I(0)), ("bDaveSmithFilter", P.B(False)),
        ("daveSmithRouteMode", P.S("DaveSmithRouteMode_Master")), ("daveSmithSendMode", P.S("DaveSmithSendMode_PreFx")),
        ("daveSmithSendTrack", P.I(-1)), ("bSoftLimitter", P.B(False)), ("bPlayArrangement", P.B(False)),
        ("swingRate", P.I(0) if obs else P.D(0.0)), ("projectBpm", P.D(120.0)),
        ("patternQuantizeUnit", P.I(packed(1, 1)) if obs else P.S("Pattern")),
        ("padQuantizeUnit", P.I(packed(1, 16)) if obs else P.S("Off")), ("padSequenceStart", P.B(False)), ("sceneIndex", P.I(0)),
        ("patternIndex", P.I(0)), ("projectFilePath", P.S(f"/mnt/emmc/{ROOT_FOLDER}/Projects/{name}.prj")),
        ("versionNo", P.I(desc.get("version", [1, 60])[0])), ("minorVersionNo", P.I(desc.get("version", [1, 60])[1]))])
    apply_props(proj, desc.get("project"))
    scenes = desc.get("scenes", {})
    omit = set(desc.get("omitScenes", []))
    for s in range(16):
        if s + 1 in omit:
            continue
        sd = scenes.get(str(s + 1), {})
        scene = P.Node(f"SceneData{s}", [("colourIndex", P.I(-1 if obs else s)), ("name", P.S("")), ("bpm", P.D(0.0))])
        apply_props(scene, sd.get("props"))
        tracks = sd.get("tracks", {})
        for t in range(16):
            tr = track(t)
            for node_name, props in tracks.get(str(t + 1), {}).items():
                apply_props(tr if node_name == "TrackData" else find(tr, node_name), props)
            scene.children.append(tr)
        pats = {k: {} for k in range(desc.get("defaultPatterns", 4))}
        for k in sd.get("dropPatterns", []):
            pats.pop(int(k) - 1, None)
        for k, pd in sd.get("patterns", {}).items():
            pats[int(k) - 1] = pd
        for k in sorted(pats):
            pd = pats[k]
            recs = step_records(pd.get("steps", []))
            if "fill" in desc:
                recs.update(fill_records(desc["fill"], s * 16 + k))
            pat = pattern(pattern_name(style, s, k), pd.get("Length", 16), recs,
                          pd.get("mutes", []), pd.get("solos", []))
            apply_props(pat, pd.get("props"))
            scene.children.append(pat)
        send, master = rename(fx("Delay"), "SendFx"), rename(fx("Off"), "MasterFx")
        apply_props(send, sd.get("SendFx"))
        apply_props(master, sd.get("MasterFx"))
        if obs:
            routing = P.Node("RoutingData", [
                ("assignMode", P.S("Master")), ("bDaveSmithFilter", P.B(False)),
                ("daveSmithRouteMode", P.S("DaveSmithRouteMode_Master")),
                ("daveSmithSendMode", P.S("DaveSmithSendMode_PreFx")), ("daveSmithSendTrack", P.I(0)),
                ("effectMode", P.S("EffectModeSend")), ("headphoneOutCh", P.S("CueMaster"))],
                [P.Node(f"TrackRoutingData{t}", [("assignableOutCh", P.S("AssignToNull"))]) for t in range(16)])
        else:
            routing = P.Node("RoutingData", [("assignMode", P.S("Master")), ("headphoneOutCh", P.S("CueMaster")),
                                             ("effectMode", P.S("Both"))],
                             [P.Node(f"TrackRoutingData{t}", [("outputAssign", P.S("Master"))]) for t in range(16)])
        apply_props(routing, sd.get("RoutingData"))
        scene.children += [send, master, routing]
        proj.children.append(scene)
    arr = desc.get("arrangement", {})
    proj.children.append(P.Node("ArrangementData0", [
        ("length", P.I(arr.get("length", 1))),
        ("Sections", P.BIN(P.sections_blob({int(k): v for k, v in arr.get("records", {}).items()})))]))
    ribbon = P.Node("RibbonData", [], [
        P.Node(f"UserSetting{u}", [], [P.Node(f"Item{i}", [("topValue", P.VOID), ("bottomValue", P.VOID)] +
                                              ([("target", P.I(255))] if obs else []))
                                       for i in range(8)]) for u in range(2)])
    ribbon.children.append(P.Node("PitchSetting", [("topCent", P.I(1200)), ("bottomCent", P.I(-1200))] if obs else
                                  [("topValue", P.I(1200)), ("bottomValue", P.I(-1200))]))
    proj.children.append(ribbon)
    port = lambda t: P.Node(t, [("bUseClock", P.B(False)), ("bUseStartStop", P.B(False)), ("bUseNoteCC", P.B(True)),
                                ("bUseSongPosition", P.B(False)), ("channelNo", P.A(P.I(c) for c in range(16)))])
    proj.children.append(P.Node("MidiSettingData", [], [port("Input"), port("Output")]))
    for e in desc.get("removeProps", []):
        find(proj, e["path"]).remove(e["name"])
    for e in desc.get("extraProps", []):
        find(proj, e["path"]).set(e["name"], e["value"])
    for e in desc.get("extraNodes", []):
        find(proj, e["path"]).children.append(node_from(e["node"]))
    return proj


def write_wav(path, freq, ms, gain, rate=44100):
    n = int(rate * ms / 1000)
    frames = bytearray()
    for i in range(n):
        env = math.exp(-5.0 * i / n) if i > 32 else i / 32.0  # 0.7 ms attack: a sharp, clean onset
        frames += struct.pack("<h", int(32767 * gain * env * math.sin(2 * math.pi * freq * i / rate)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("args", nargs="+", metavar="DESC.json OUT_DIR | OUT.prj")
    ap.add_argument("--name")
    ap.add_argument("--default", action="store_true", help="write a default project to OUT (a .prj path)")
    ap.add_argument("--print", action="store_true", help="print the tree as JSON")
    a = ap.parse_args()
    if a.default:
        data = P.container(build({}, "Default"))
        pathlib.Path(a.args[0]).write_bytes(data)
        print(a.args[0])
        return 0
    if len(a.args) != 2:
        ap.error("DESC.json OUT_DIR expected")
    desc = json.loads(pathlib.Path(a.args[0]).read_text())
    name = a.name or desc.get("name", pathlib.Path(a.args[0]).stem)
    root = pathlib.Path(a.args[1]) / ROOT_FOLDER
    for d in ("Projects", "Samples", "System", "RenderedAudio", "Scenes"):
        (root / d).mkdir(parents=True, exist_ok=True)
    tree = build(desc, name)
    data = P.container(tree)
    prj = root / "Projects" / f"{name}.prj"
    prj.write_bytes(data)
    (root / "System" / "data.rsm").write_bytes(data)
    for fname, sd in desc.get("samples", {}).items():
        dest = root / "Samples" / fname
        dest.parent.mkdir(parents=True, exist_ok=True)
        write_wav(dest, sd.get("freq", 100), sd.get("ms", 100), sd.get("gain", 0.8))
        if "dat" in sd:
            d = sd["dat"]
            pathlib.Path(str(dest) + ".dat").write_bytes(P.wav_dat(d.get("bar", 0.0), d.get("bpm", 0.0),
                                                                     d.get("sampleNum", 0)))
    if a.print:
        print(json.dumps(P.node_json(tree), indent=1))
    print(prj)
    return 0


if __name__ == "__main__":
    sys.exit(main())
