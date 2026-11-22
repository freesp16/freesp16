"""Multi-mode filter goldens (spec 02-features/effects/multimode-filter.md "Test notes"): the research
model `tools/fx/mmf.py` (MIT; + lofi.py, crmath.py, fdlibm_powf.py), optionally inside the research wrapper
`tools/fx/effectbase.py`. C++ subject: fx.filter (tests/golden/harness/FiltSubject.cpp: engine::fx::effects::
MultiModeFilter in the Reference quirk profile, alone or inside engine::fx::FxShell).

params (app terms — the adapter maps them to the model's indices and enum values):
  "fs": the recipe's sample rate (the C++ subject checks)
  "mode": "effect" (default: the effect's own output, slot 25) | "shell" (EffectBase / FxShell around it: activation at
          block 0, crossfade dry 1 - v / wet v from k5, the 10 Hz wet high-pass)
  "init": {"k": {"<0..5>": v}, "type": t, "slope": s}   on top of the insert defaults, before the activation reset
  "defaults": false   skip the insert defaults (only "init" is set: the constructor values stay, e.g. FX-581's trap)
  "events": [{"block": b, "k": {...}, "type": t, "slope": s, "reset": true}]   applied before block b (reset = slot 27)
  "walk": {"seed": n (!= 0), "from": b0, "to": b1, "keys": ["0", ..., "type", "slope"]}   a seeded random walk: at
          every block b0 <= b < b1 and for each key in order, xorshift32 draws r; when r % 4 == 0 a second draw x sets
          the key (knob: f32((x >> 8) * 2^-24); type: x % 4; slope: x % 2). Applied after the block's events.
  type: 0 LowPass, 1 HighPass, 2 BandPass, 3 Off (-> FILTERTYPE 0/1/2/6); slope: 0 = 12 dB, 1 = 24 dB (-> FILTERDB 1/2).
Inputs: L, R. Outputs: L, R.
"""
from ..f32 import F

NAME = "fx_filter"
SUBJECT = "fx.filter"
RESEARCH_MODELS = ["mmf", "effectbase"]
OWNER_INPUTS = []
KEYS = {"0", "1", "2", "3", "4", "5"}


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def ref_type(t):
    t = int(t)
    return t if t in (0, 1, 2) else 6


def ref_slope(s):
    return 1 if int(s) <= 0 else 2


def xorshift32(state):
    x = state[0]
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    state[0] = x & 0xffffffff
    return state[0]


def check_params(p, extra_keys=()):
    if p.get("mode", "effect") not in ("effect", "shell"):
        raise ValueError("mode: %r" % p.get("mode"))
    allowed = {"k"} | set(extra_keys)
    for item in [p.get("init", {})] + list(p.get("events", [])):
        bad = set(item) - allowed - {"block", "reset"}
        if bad:
            raise ValueError("unknown keys %s in %r" % (sorted(bad), item))
        if set(item.get("k", {})) - KEYS:
            raise ValueError("knob keys are '0'..'5': %r" % item)
    w = p.get("walk")
    if w is not None and (int(w.get("seed", 0)) == 0 or set(w) - {"seed", "from", "to", "keys"}):
        raise ValueError("walk: {seed != 0, from, to, keys}: %r" % w)


def walk_events(p):
    """block -> list of (key, value) of the seeded walk (same law as the C++ subject)."""
    w = p.get("walk")
    out = {}
    if not w:
        return out
    st = [int(w["seed"]) & 0xffffffff]
    for b in range(int(w["from"]), int(w["to"])):
        for key in w["keys"]:
            r = xorshift32(st)
            if r % 4 != 0:
                continue
            x = xorshift32(st)
            out.setdefault(b, []).append((key, x))
    return out


def walk_value(key, x, enum_sizes):
    if key in enum_sizes:
        return x % enum_sizes[key]
    return F((x >> 8) * (1.0 / 16777216.0))


def validate(case):
    check_params(case["params"], ("type", "slope"))


def apply(m, item):
    for k, v in item.get("k", {}).items():
        m.set_param(int(k), F(v))
    if "type" in item:
        m.set_param(6, ref_type(item["type"]))
    if "slope" in item:
        m.set_param(7, ref_slope(item["slope"]))


def drive(case, inputs, research, block, make, apply_fn, walk_apply, hpf, mix_hook):
    """Shared driver of fx_filter / fx_eq2: builds the model, applies init, events and the walk, runs every block."""
    p = case["params"]
    fs = int(float(p.get("fs", 44100)))
    m = make(fs)
    if p.get("defaults", True):
        m.insert_defaults()
    apply_fn(m, p.get("init", {}))
    walk = walk_events(p)
    L, R = inputs
    oL, oR = list(L), list(R)
    shell = p.get("mode", "effect") == "shell"
    eb = None
    if shell:
        eb_mod = research.load("effectbase")

        def s25(e, bl, br, n):
            yl, yr = m.process(bl, br)
            bl[:] = yl
            br[:] = yr
            mix_hook(m, e, False)
            return 0

        def s26(e):
            mix_hook(m, e, True)

        def s27(e):
            m.reset()

        eb = eb_mod.EffectBase(fs=fs, block=block, mode=0, slot25=s25, slot26=s26, slot27=s27)
        eb.slot22(0)
        eb.set_post_hpf(hpf, 10.0)
        eb.slot5(0.0, 1)
        eb.slot7()
    else:
        m.reset()
    for b in range(len(L) // block):
        for ev in p.get("events", []):
            if ev["block"] != b:
                continue
            apply_fn(m, ev)
            if ev.get("reset"):
                m.reset()
        for key, x in walk.get(b, []):
            walk_apply(m, key, x)
        bl = oL[b * block:(b + 1) * block]
        br = oR[b * block:(b + 1) * block]
        if shell:
            eb.slot18(bl, br, block)
        else:
            bl, br = m.process(bl, br)
        oL[b * block:(b + 1) * block] = bl
        oR[b * block:(b + 1) * block] = br
    return [oL, oR]


def run(case, inputs, research, block):
    mmf = research.load("mmf")

    def walk_apply(m, key, x):
        v = walk_value(key, x, {"type": 4, "slope": 2})
        if key == "type":
            m.set_param(6, ref_type(v))
        elif key == "slope":
            m.set_param(7, ref_slope(v))
        else:
            m.set_param(int(key), v)

    def mix_hook(m, e, at_activation):
        # slot 26 (0x107b30): wet = v, dry = 1 - v when MIXRATIO changed; mmf.process does it after slot 25 itself
        if at_activation and m.params[5].changed():
            v = m.params[5].get()
            m.wet = v
            m.dry = F(F(1.0) - v)
        if m.wet is not None:
            e.dry, e.wet = m.dry, m.wet

    return drive(case, inputs, research, block, lambda fs: mmf.MMF(fs=fs, block=block), apply, walk_apply, True,
                 mix_hook), {}
