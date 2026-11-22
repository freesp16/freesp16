"""Mixer golden: the research track strip and bus chain (`tools/fx/track_mix.py` Track + sum_tracks, `tools/fx/bus_mix.py`
Fader / SendUnit / MasterOut; both MIT) with the app's gain table, driving engine::mixer::Mixer.

Per case: N tracks, each `{"fader": 0..127, "pan": -64..64, "send": 0..127, "main_on": bool}`, and events
`{"block": b, "track": t, "offset": 0..63, "fader"|"pan"|"send": v}` (a 100-sample ramp starting at `offset`) or
`{"block": b, "track": t, "offset": 0..63, "main_on": bool}` (128-sample mute ramp from `offset`), applied before block b
is processed. Inputs: t<k>.L, t<k>.R per track. Outputs: main.L/R = sum of the faded mains, send.L/R = sum of the send
taps (post-insert, post-pan, post-mute, pre-fader, GainLaw x 0.5); no inserts.

Optional `"bus": {"master": 0..127, "return": 0..127, "send_fx": "off"|"half", "master_fx": "none"|"swap"}` adds the
outputs master.L/R: main bus -> Master FX stand-in -> + (send bus -> Send FX stand-in -> return fader) -> master fader
(bus_mix.BusChain order: the return is added after the master FX). Stand-ins: "half" = x * 0.5, "swap" = (R, L),
"off" = Send FX Off (the return is silence, audio-engine.md "Buses"). Bus events: `{"block": b, "offset": o,
"master"|"return": v}`.

Changes to the models, all in this adapter (the research files are unchanged):
  - the model's VOLUME_TABLE (reference ladder for indices 1-9) is replaced by core::GainLaw's table;
  - start-up (our choice): every smoother and mute starts *settled* at the case's initial values, as the
    app's Mixer does after prepare() (the model's smoothers start at 1.0 and its mute fades in over 128 samples);
  - a mute change at an offset > 0 splits the block there (the model's MuteSmoother switches only at block starts);
    gain events keep their absolute offsets (the model's smoother counts its offset across the split).
"""
from .. import applaw
from ..f32 import F, bits

NAME = "track_strip"
SUBJECT = "mixer.track_strip"
RESEARCH_MODELS = ["track_mix", "bus_mix"]
OWNER_INPUTS = []
KEYS = ("fader", "pan", "send")
BUS_KEYS = ("master", "return")


def _tracks(case):
    return case["params"]["tracks"]


def input_channels(case):
    return ["t%d.%s" % (k, s) for k in range(len(_tracks(case))) for s in ("L", "R")]


def output_channels(case):
    out = ["main.L", "main.R", "send.L", "send.R"]
    if "bus" in case["params"]:
        out += ["master.L", "master.R"]
    return out


def validate(case):
    p = case["params"]
    if set(p) - {"tracks", "events", "bus"}:
        raise ValueError("unknown params %s" % sorted(set(p) - {"tracks", "events", "bus"}))
    for t in p["tracks"]:
        unknown = set(t) - {"fader", "pan", "send", "main_on"}
        if unknown:
            raise ValueError("unknown track keys %s" % sorted(unknown))
    bus = p.get("bus")
    if bus is not None:
        if set(bus) - {"master", "return", "send_fx", "master_fx"}:
            raise ValueError("unknown bus keys %r" % bus)
        if bus.get("send_fx", "off") not in ("off", "half") or bus.get("master_fx", "none") not in ("none", "swap"):
            raise ValueError("unknown FX stand-in %r" % bus)
    for ev in p.get("events", []):
        if not (0 <= ev.get("offset", 0) < case["_block"]):
            raise ValueError("event offset must be inside the block: %r" % ev)
        keys = set(ev) - {"block", "track", "offset"}
        if keys <= set(BUS_KEYS) and "track" not in ev:
            if bus is None:
                raise ValueError("bus event without a bus: %r" % ev)
            continue
        if not (0 <= ev["track"] < len(p["tracks"])):
            raise ValueError("event track out of range: %r" % ev)
        if not keys <= set(KEYS) | {"main_on"}:
            raise ValueError("unknown event keys: %r" % ev)


def _settle(tm, tr):
    gl, gr = tm.pan_gains(tr.pan)
    tr.sm_fader = tm.Smoother(tm.Track.RAMP, tm.fader_gain(tr.fader))
    tr.sm_pan_l = tm.Smoother(tm.Track.RAMP, gl)
    tr.sm_pan_r = tm.Smoother(tm.Track.RAMP, gr)
    tr.sm_send = tm.Smoother(tm.Track.RAMP, tm.knob_gain(tr.send))
    tr.mute_main = tm.MuteSmoother(128, on=tr.main_on)


def _stand_in(name):
    if name == "half":
        return lambda L, R: ([F(x * 0.5) for x in L], [F(x * 0.5) for x in R])
    if name == "swap":
        return lambda L, R: (list(R), list(L))
    if name == "off":
        return lambda L, R: ([0.0] * len(L), [0.0] * len(R))
    return None


def run(case, inputs, research, block):
    tm = research.load("track_mix")
    bm = research.load("bus_mix")
    table = applaw.gain_table()
    tm.VOLUME_TABLE = list(table)          # plug the app's gain law (fader_gain / knob_gain read the module global)
    p = case["params"]
    tracks = [tm.Track(fader=t.get("fader", 100), pan=t.get("pan", 0), send=t.get("send", 0),
                       main_on=t.get("main_on", True)) for t in p["tracks"]]
    for tr in tracks:
        _settle(tm, tr)
    bus = p.get("bus")
    if bus is not None:
        send = bm.SendUnit(_stand_in(bus.get("send_fx", "off")), bus.get("return", 100))
        mout = bm.MasterOut(bus.get("master", 100))
        for f in (send.ret, mout.fader):
            f.sm = tm.Smoother(bm.RAMP, tm.fader_gain(f.index))
        master_fx = _stand_in(bus.get("master_fx", "none"))
    events = sorted(p.get("events", []), key=lambda e: e["block"])
    frames = len(inputs[0]) if inputs else 0
    out = [[] for _ in range(6 if bus is not None else 4)]
    for b in range(frames // block):
        evs = [e for e in events if e["block"] == b]
        cuts = sorted({e.get("offset", 0) for e in evs if "main_on" in e and e.get("offset", 0) > 0})
        bounds = [0] + cuts + [block]
        segs = [[[] for _ in range(4)] for _ in tracks]
        for a, z in zip(bounds, bounds[1:]):
            for ev in evs:
                off = ev.get("offset", 0)
                if "track" not in ev:
                    continue
                tr = tracks[ev["track"]]
                if a == 0:
                    kw = {k: ev[k] for k in KEYS if k in ev}
                    if kw:
                        tr.set(offset=off, **kw)
                if "main_on" in ev and off == a:
                    tr.main_on = bool(ev["main_on"])
            s = slice(b * block + a, b * block + z)
            for k, tr in enumerate(tracks):
                o = tr.process(inputs[2 * k][s], inputs[2 * k + 1][s])
                for c in range(4):
                    segs[k][c].extend(o[c])
        ml, mr = tm.sum_tracks(segs, (0, 1))
        sl, sr = tm.sum_tracks(segs, (2, 3))
        for c, v in zip(out, (ml, mr, sl, sr)):
            c.extend(v)
        if bus is not None:
            for ev in evs:
                if "track" in ev:
                    continue
                if "master" in ev:
                    mout.fader.set(ev["master"], ev.get("offset", 0))
                if "return" in ev:
                    send.ret.set(ev["return"], ev.get("offset", 0))
            mains = master_fx(ml, mr) if master_fx else (ml, mr)
            ret = send.process(list(sl), list(sr))
            mo = mout.process([mains[0], mains[1], ret[0], ret[1], [0.0] * block, [0.0] * block])
            out[4].extend(mo[0])
            out[5].extend(mo[1])
    tables = {"gain_law": {"source": "core/util/GainLaw.h (GainLaw::gain, index 0..127)",
                           "f32_bits": [bits(g) for g in table]}}
    return out, tables
