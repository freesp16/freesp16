"""Run a delay/reverb research model through the research wrapper `tools/fx/effectbase.py` (EffectBase, MIT), the way the
C++ subject fx.delay (tests/golden/harness/DelaySubject.cpp) runs an engine::fx effect through engine::fx::FxShell in the
Reference quirk profile (FX-052). Like goldenlib/effectrun.py (processor wrapper state: slot22(0), gate
mode and post high-pass from the shell policy, activation = slot5 + slot7, stop = slot6), plus the slot kind and the
processors' tempo feed: before every block the tempo is written as parameter 7 (the Insert, Send and Master processors
do it for the delays, F017/F115; the reverb gets none, F123). As in the C++ subject, the activation's reset runs before
the first feed (it sees the constructor's 120 BPM, or the previous block's tempo).

Case parameters:
  {"fs": Hz (default 44100), "slot": "insert" | "send" | "master" (default insert; picks the app's defaults),
   "bpm": BPM (default 120),
   "init": {"k": v, ...}            knob values before the first block (k = internal index 0..5),
   "options": {"i": n, ...}         option values before the first block (i = the descriptor's option index),
   "activate": true                 activate before block 0 (default true),
   "events": [{"block": b, "k": k, "v": v} | {"block": b, "option": i, "value": n} |
              {"block": b, "op": "activate" | "stop"} | {"block": b, "bpm": BPM}]}      applied in list order before b
The adapter supplies `effect`: set_param(k, v), set_option(i, n), set_bpm(bpm) (no-op for the reverb), process(L, R)
(in place), reset(), dry_wet() -> (dry, wet) and the policy fields gate (0/1) and hpf (bool).
"""
from .f32 import F

EVENT_KEYS = ({"block", "k", "v"}, {"block", "option", "value"}, {"block", "op"}, {"block", "bpm"})
SLOTS = ("insert", "send", "master")


def validate(case, n_options, slots=SLOTS, extra_keys=()):
    p = case["params"]
    allowed = {"effect", "fs", "slot", "bpm", "init", "options", "activate", "events"} | set(extra_keys)
    unknown = set(p) - allowed
    if unknown:
        raise ValueError("unknown case parameters %s" % sorted(unknown))
    if p.get("slot", "insert") not in slots:
        raise ValueError("slot must be one of %s: %r" % (slots, p.get("slot")))
    for k in p.get("init", {}):
        if not 0 <= int(k) < 6:
            raise ValueError("init keys are knob indices 0..5: %r" % k)
    for i in p.get("options", {}):
        if not 0 <= int(i) < max(n_options, 0) or n_options == 0:
            raise ValueError("options keys are option indices 0..%d: %r" % (n_options - 1, i))
    for ev in p.get("events", []):
        if set(ev) not in EVENT_KEYS:
            raise ValueError("events are {block, k, v} | {block, option, value} | {block, op} | {block, bpm}: %r" % ev)
        if "op" in ev and ev["op"] not in ("activate", "stop"):
            raise ValueError("event op is activate | stop: %r" % ev)


def run(case, inputs, research, block, effect):
    eb_mod = research.load("effectbase")
    p = case["params"]
    fs = int(p.get("fs", 44100))
    bpm = [float(p.get("bpm", 120.0))]

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
                activate() if ev["op"] == "activate" else eb.slot6(1.0)
            elif "bpm" in ev:
                bpm[0] = float(ev["bpm"])
            elif "k" in ev:
                effect.set_param(int(ev["k"]), F(ev["v"]))
            else:
                effect.set_option(int(ev["option"]), int(ev["value"]))
        effect.set_bpm(F(bpm[0]))                       # the processor's tempo feed, every block
        bl = oL[b * block:(b + 1) * block]; br = oR[b * block:(b + 1) * block]
        eb.slot18(bl, br, block)
        oL[b * block:(b + 1) * block] = bl; oR[b * block:(b + 1) * block] = br
    return [oL, oR]
