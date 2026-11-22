"""2-band EQ goldens (spec 02-features/effects/parametric-eq.md "Test notes"): the research model
`tools/fx/peq.py` (MIT; + lofi.py, mmf.py, crmath.py, fdlibm_powf.py), optionally inside the research wrapper
`tools/fx/effectbase.py`. C++ subject: fx.eq2 (tests/golden/harness/FiltSubject.cpp: engine::fx::effects::ParametricEq
in the Reference quirk profile, alone or inside engine::fx::FxShell).

params: as models/fx_filter.py ("fs", "mode", "init", "events", "walk"), with
  knobs "0".."5" = B1 frequency, B1 gain, B1 Q, B2 frequency, B2 gain, B2 Q (the internal index k);
  "type1" / "type2" = the band types in the app's enum (UI order of FX-069): 0 HPF, 1 LPF, 2 PEAK, 3 HI-SHELF,
  4 LO-SHELF (-> the model's BAND1TYPE / BAND2TYPE = 4 - t); walk keys "type1"/"type2" draw x % 5;
  "parity_maths": true -> the C++ effect computes the frequency law with its fdlibm powf port (FX-054; the model always
  does): such cases can be bit-exact ("parity": true); without it the default maths (std::pow) is checked at 1e-4.
  The shell mode wraps it with dry 0 / wet 1 and no high-pass (FX-461).
Inputs: L, R. Outputs: L, R.
"""
from ..f32 import F
from . import fx_filter as common

NAME = "fx_eq2"
SUBJECT = "fx.eq2"
RESEARCH_MODELS = ["peq", "effectbase"]
OWNER_INPUTS = []


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def ref_type(t):
    t = int(t)
    t = 0 if t < 0 else (4 if t > 4 else t)
    return 4 - t


def validate(case):
    p = case["params"]
    common.check_params(p, ("type1", "type2"))
    if set(p) - {"fs", "mode", "init", "events", "walk", "parity_maths", "defaults"}:
        raise ValueError("fx_eq2 params: %s" % sorted(p))


def apply(m, item):
    for k, v in item.get("k", {}).items():
        m.set_param(int(k), F(v))
    if "type1" in item:
        m.set_param(6, ref_type(item["type1"]))
    if "type2" in item:
        m.set_param(7, ref_type(item["type2"]))


def run(case, inputs, research, block):
    peq = research.load("peq")

    def walk_apply(m, key, x):
        v = common.walk_value(key, x, {"type1": 5, "type2": 5})
        if key in ("type1", "type2"):
            m.set_param(6 if key == "type1" else 7, ref_type(v))
        else:
            m.set_param(int(key), v)

    def mix_hook(m, e, at_activation):
        e.dry, e.wet = F(0.0), F(1.0)   # slot 26 (0x10a4d8): dry 0, wet 1, every block and at activation

    return common.drive(case, inputs, research, block, lambda fs: peq.PEQ(fs=fs, block=block), apply, walk_apply,
                        False, mix_hook), {}
