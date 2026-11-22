"""Ducker goldens (spec 02-features/effects/ducker.md "Test notes"): the research model
`tools/fx/ducker.py` (MIT; class Ducker, slot 25 `process`, slot 27 `reset`) in FREE mode, driven per block by the
host rule of research docs/effects/ducker.md "The trigger path" (F201): set_param(7, bpm), set_param(8, flag),
set_param(9, offset), then process. C++ subject: fx.ducker (tests/golden/harness/DuckerSubject.cpp:
engine::fx::effects::Ducker through EffectBase::process with the trigger slot in the ProcessContext, Reference
quirk profile).

Sync mode is not golden-comparable (the app's own tables, FX-1210): tests/unit/fx/test_fx_ducker.cpp checks it by
formula. Patterns keep at least one empty block between two hits (FX-1280 differs in the Sixteen profile).

params (step fractions as stored, k = the model's parameter index; omitted = the Insert defaults of the model,
INSERT_DEFAULTS: attack 0.03, curves 0.5, release 2999/79999, depth 1.0):
  {"fs": 44100, "bpm": 120.0, "attack": v, "attack_curve": v, "release": v, "release_curve": v, "depth": v,
   "triggers": [{"block": b, "offset": o}],            the source track's trigger slot in block b (else flag 0, offset 0)
   "events": [{"block": b, "k": 0..5, "v": v}],         a parameter write before block b's host rule
   "resets": [b]}                                       slot 27 (reset) before block b's host rule
Inputs and outputs: L, R (the Ducker is wet only: dry 0, wet 1).
"""
from ..f32 import F

NAME = "ducker"
SUBJECT = "fx.ducker"
RESEARCH_MODELS = ["ducker"]
OWNER_INPUTS = []
KNOBS = {"attack": 0, "attack_curve": 1, "release": 2, "release_curve": 3, "depth": 5}


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    p = case["params"]
    extra = set(p) - set(KNOBS) - {"fs", "bpm", "triggers", "events", "resets"}
    if extra:
        raise ValueError("ducker params: unknown keys %s" % sorted(extra))
    for t in p.get("triggers", []):
        if set(t) != {"block", "offset"} or not 0 <= t["offset"] <= 63:
            raise ValueError("ducker triggers are {block, offset 0..63}: %r" % t)
    blocks = sorted(t["block"] for t in p.get("triggers", []))
    for a, b in zip(blocks, blocks[1:]):
        if b - a < 2:
            raise ValueError("ducker triggers need an empty block in between (FX-1280): blocks %d, %d" % (a, b))
    for ev in p.get("events", []):
        if set(ev) != {"block", "k", "v"} or ev["k"] not in (0, 1, 2, 3, 5):
            raise ValueError("ducker events are {block, k in 0,1,2,3,5, v}: %r" % ev)


def run(case, inputs, research, block):
    dm = research.load("ducker")
    p = case["params"]
    d = dm.Ducker(fs=int(p.get("fs", 44100)))
    d.insert_defaults(own_track=0)
    for key, k in KNOBS.items():
        if key in p:
            d.set_param(k, F(p[key]))
    bpm = F(float(p.get("bpm", 120.0)))
    trig = {t["block"]: t["offset"] for t in p.get("triggers", [])}
    L, R = inputs
    oL, oR = [], []
    for b in range(len(L) // block):
        if b in p.get("resets", []):
            d.reset()
        for ev in p.get("events", []):
            if ev["block"] == b:
                d.set_param(ev["k"], F(ev["v"]))
        d.set_param(7, bpm)
        d.set_param(8, 1.0 if b in trig else 0.0)
        d.set_param(9, float(trig.get(b, 0)))
        bl, br = d.process(L[b * block:(b + 1) * block], R[b * block:(b + 1) * block])
        oL.extend(bl); oR.extend(br)
    return [oL, oR], {}
