"""Run a tempo-fed, retriggerable research effect model (Chorus, Flanger, Phaser) through the research
wrapper `tools/fx/effectbase.py` (EffectBase, MIT), the way the C++ subject fx.mod (tests/golden/harness/ModSubject.cpp)
runs an engine::fx effect through engine::fx::FxShell in the Reference quirk profile (FX-052). Same wrapper state as
goldenlib/effectrun.py (Insert processor, slot22(0): no skip/clamp, gate mode and the 10 Hz post high-pass from the
effect's shell policy; activation = slot5(0.0, 1) + slot7(), stop = slot6(1.0)), plus:

  - the tempo (FX-049): the model's BPM parameter (index 7) is set before every block, as the processor feeds it;
  - retrigger (FX-089/090): the model's slot 10 (the C++ subject calls EffectBase::retrigger()).

Case parameters:
  {"fs": Hz (optional; default 44100 — the subject uses it too),
   "bpm": initial tempo (default 120; 1…999, FX-049's clamp is the caller's),
   "parity_maths": bool (Phaser only: the C++ effect uses its fdlibm tanf/powf ports, FX-054; the model always does),
   "init": {"k": v, ...}          knob values before the first block (k = internal index 0..5; the rest = app defaults),
   "options": {"i": n, ...}       option values before the first block (i = the descriptor's option index; 0 = SYNC),
   "activate": true               activate before block 0 (default true),
   "events": [{"block": b, "k": k, "v": v} | {"block": b, "option": i, "value": n} | {"block": b, "bpm": x} |
              {"block": b, "op": "activate" | "stop" | "retrigger"}]}    applied in list order before block b
The adapter supplies `effect`: set_param(k, v), set_option(i, n), set_bpm(x), retrigger(), process(L, R) (in place),
reset(), dry_wet() -> (dry, wet) and the policy fields gate (0/1) and hpf (bool).
"""
from .f32 import F

EVENT_KEYS = ({"block", "k", "v"}, {"block", "option", "value"}, {"block", "op"}, {"block", "bpm"})
PARAM_KEYS = {"fs", "bpm", "parity_maths", "init", "options", "activate", "events", "effect"}


def validate(case, n_options, effect_id):
    p = case["params"]
    if p.get("effect", effect_id) != effect_id:
        raise ValueError("%s recipe: params.effect must be %s" % (effect_id, effect_id))
    extra = set(p) - PARAM_KEYS
    if extra:
        raise ValueError("unknown params %s (allowed: %s)" % (sorted(extra), sorted(PARAM_KEYS)))
    for k in p.get("init", {}):
        if not 0 <= int(k) < 6:
            raise ValueError("init keys are knob indices 0..5: %r" % k)
    for i in p.get("options", {}):
        if not 0 <= int(i) < n_options:
            raise ValueError("options keys are option indices 0..%d: %r" % (n_options - 1, i))
    for ev in p.get("events", []):
        if set(ev) not in EVENT_KEYS:
            raise ValueError("events are {block, k, v} | {block, option, value} | {block, bpm} | {block, op}: %r" % ev)
        if "op" in ev and ev["op"] not in ("activate", "stop", "retrigger"):
            raise ValueError("event op is activate | stop | retrigger: %r" % ev)
        if "bpm" in ev and not 1.0 <= float(ev["bpm"]) <= 999.0:
            raise ValueError("bpm events are 1..999 (FX-049): %r" % ev)
    if not 1.0 <= float(p.get("bpm", 120.0)) <= 999.0:
        raise ValueError("bpm is 1..999 (FX-049)")


def run(case, inputs, research, block, effect):
    eb_mod = research.load("effectbase")
    p = case["params"]
    fs = int(p.get("fs", 44100))

    def s25(eb, L, R, n):
        effect.process(L, R)
        eb.dry, eb.wet = effect.dry_wet()
        return 0

    def s26(eb):
        eb.dry, eb.wet = effect.dry_wet()

    def s27(eb):
        effect.reset()
        return 0

    eb = eb_mod.EffectBase(fs=fs, block=block, mode=effect.gate, slot25=s25, slot26=s26, slot27=s27)
    eb.slot22(0)
    eb.set_post_hpf(bool(effect.hpf), 10.0)
    for k, v in sorted(p.get("init", {}).items(), key=lambda kv: int(kv[0])):
        effect.set_param(int(k), F(v))
    for i, n in sorted(p.get("options", {}).items(), key=lambda kv: int(kv[0])):
        effect.set_option(int(i), int(n))
    bpm = F(float(p.get("bpm", 120.0)))

    def activate():
        eb.slot5(0.0, 1)
        eb.slot7()

    if p.get("activate", True):
        activate()
    L, R = inputs
    oL, oR = list(L), list(R)
    for b in range(len(L) // block):
        for ev in p.get("events", []):
            if ev["block"] != b:
                continue
            if "op" in ev:
                if ev["op"] == "activate":
                    activate()
                elif ev["op"] == "stop":
                    eb.slot6(1.0)
                else:
                    effect.retrigger()
            elif "bpm" in ev:
                bpm = F(float(ev["bpm"]))
            elif "k" in ev:
                effect.set_param(int(ev["k"]), F(ev["v"]))
            else:
                effect.set_option(int(ev["option"]), int(ev["value"]))
        effect.set_bpm(bpm)
        bl = oL[b * block:(b + 1) * block]; br = oR[b * block:(b + 1) * block]
        eb.slot18(bl, br, block)
        oL[b * block:(b + 1) * block] = bl; oR[b * block:(b + 1) * block] = br
    return [oL, oR]
