"""Compressor "app-maths" goldens (spec 02-features/effects/compressor.md "Test notes"): the research model
`tools/fx/compressor.py` (MIT; `Compressor.process` = Insert slot 25, `process_send` = Send/Master slot 25 with the
2 ms look-ahead) with the correctly rounded `crmath.expf` (MIT) in place of `glibc_expf.expf`.

The model needs the glibc 2.13 `expf` tables (firmware-derived, user-supplied) for bit-exactness. This adapter never
loads them and never imports the LGPL `glibc_expf.py`: before the model is imported, a stand-in module named
`glibc_expf` (expf = crmath.expf, loaded() = True, load_tables refused) is placed in sys.modules, and removed after.
The result differs from the device by ≤ 1 ulp in the slew factors a, r and e, so the comparison runs at the default-
maths tolerance (output ≤ 1e-4; the C++ default build uses std:: maths in double anyway). CI uses this substitution
(the recipe is not "local_only"); the Compressor's behavioural set (tests/golden/harness/selftest/
test_compressor_behaviour.cpp) runs in CI as well.

C++ subject: fx.compressor (tests/golden/harness/CompressorSubject.cpp). Output = the slot-18 mix with the post-stage
neutral: out = F(F(in·dry) + F(y·wet)), dry = F(1 − Mix), wet = F(Mix·makeup) after each block (slot 26).

params (step fractions; omitted = the model's Insert defaults, INSERT_DEFAULTS: threshold 1.0, ratio 0, attack 0,
release 0, gain 0, mix 1, knee 0, detect 0):
  {"fs": 44100, "send": false, "threshold": v, "ratio": v, "attack": v, "release": v, "gain": v, "mix": v,
   "knee": 0|1, "detect": 0|1, "lookahead": 0|1,
   "events": [{"block": b, "k": 0..5 | "knee" | "detect" | "lookahead", "v": v}]}   writes before block b
Inputs and outputs: L, R.
"""
import sys
import types

from ..f32 import F

NAME = "compressor"
SUBJECT = "fx.compressor"
RESEARCH_MODELS = ["compressor", "crmath", "glibc_log10", "fdlibm_powf"]
OWNER_INPUTS = []
KNOBS = {"threshold": 0, "ratio": 1, "attack": 2, "release": 3, "gain": 4, "mix": 5}
OPTS = {"knee": 7, "detect": 8}


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    p = case["params"]
    extra = set(p) - set(KNOBS) - set(OPTS) - {"fs", "send", "lookahead", "events"}
    if extra:
        raise ValueError("compressor params: unknown keys %s" % sorted(extra))
    for ev in p.get("events", []):
        if set(ev) != {"block", "k", "v"} or ev["k"] not in (0, 1, 2, 3, 4, 5, "knee", "detect", "lookahead"):
            raise ValueError("compressor events are {block, k, v}: %r" % ev)
    if p.get("lookahead") and not p.get("send"):
        raise ValueError("look-ahead needs \"send\": true (the reference Insert ignores it, F087)")


def _stand_in(crmath):
    m = types.ModuleType("glibc_expf")
    m.expf = crmath.expf
    m.loaded = lambda: True

    def refuse(*a, **k):
        raise RuntimeError("golden-gen never loads the expf tables (firmware-derived)")
    m.load_tables = refuse
    return m


def _set(c, k, v):
    if k == "lookahead":
        c.lookahead_param = 1 if v else 0
    elif k == "knee":
        c.set_param(7, float(v))
    elif k == "detect":
        c.set_param(8, float(v))
    else:
        c.set_param(int(k), F(v))


def run(case, inputs, research, block):
    crmath = research.load("crmath")
    saved = sys.modules.get("glibc_expf")
    sys.modules["glibc_expf"] = _stand_in(crmath)
    try:
        cm = research.load("compressor")
        p = case["params"]
        send = bool(p.get("send"))
        c = cm.Compressor(fs=int(p.get("fs", 44100)), n=block, send=send)
        c.insert_defaults()
        for key, k in KNOBS.items():
            if key in p:
                c.set_param(k, F(p[key]))
        for key in ("knee", "detect", "lookahead"):
            if key in p:
                _set(c, key, p[key])
        L, R = inputs
        oL, oR = [], []
        for b in range(len(L) // block):
            for ev in p.get("events", []):
                if ev["block"] == b:
                    _set(c, ev["k"], ev["v"])
            xl = L[b * block:(b + 1) * block]; xr = R[b * block:(b + 1) * block]
            yl, yr = (c.process_send if send else c.process)(xl, xr)
            d, w = c.dry, c.wet
            oL.extend(F(F(x * d) + F(y * w)) for x, y in zip(xl, yl))
            oR.extend(F(F(x * d) + F(y * w)) for x, y in zip(xr, yr))
    finally:
        if saved is None:
            sys.modules.pop("glibc_expf", None)
        else:
            sys.modules["glibc_expf"] = saved
    return [oL, oR], {"expf": {"source": "crmath.expf (correctly rounded) in place of the glibc 2.13 expf",
                               "f32_bits": []}}
