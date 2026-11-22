"""LoFi goldens (spec lofi.md FX-920…926, FX-960, "Test notes"): the research model `tools/fx/lofi.py`
(class LoFi, MIT; with crmath.py and fdlibm_powf.py) through the research wrapper `tools/fx/effectbase.py` (Insert
wrapper state, gate mode 0, 10 Hz post high-pass on, crossfade mix: LoFi slot 26). C++ subject: fx.effect with
params.effect = "lofi" (engine::fx::LoFi in the Reference quirk profile, through engine::fx::FxShell).

Knobs k = the model's indices 0..5 (PREAMP, REDUCTION, BITDEPTH, CUTOFF, RESONANCE, MIXRATIO). Options (the
descriptor's order): 0 = filter type (HP, LP, BP, Off → FILTERTYPE 0..3), 1 = position (Pre, Post → FILTERPOSITION
0, 1). The knobs start at the app's defaults (core/params/catalogue/fx_lofi.cpp, = the model's INSERT_DEFAULTS), the
options at Off / Post. Shared case parameters: goldenlib/effectrun.py. Inputs and outputs: L, R.
"""
from .. import effectrun
from ..f32 import F

NAME = "lofi"
SUBJECT = "fx.effect"
RESEARCH_MODELS = ["lofi", "effectbase", "crmath", "fdlibm_powf"]
OWNER_INPUTS = []
APP_DEFAULTS = [0.0, 1.0, 1.0, 0.566, 5.0 / 98.0, 1.0]   # = lofi.INSERT_DEFAULTS 0..5 (FX-961)
APP_OPTION_DEFAULTS = [3, 1]                               # Off, Post


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    if case["params"].get("effect", "lofi") != "lofi":
        raise ValueError("lofi recipe: params.effect must be lofi")
    effectrun.validate(case, 2)


class _Effect:
    gate = 0
    hpf = True

    def __init__(self, mod, fs):
        self.m = mod.LoFi(fs=fs)
        for k, v in enumerate(APP_DEFAULTS):
            self.m.set_param(k, F(v))
        for i, n in enumerate(APP_OPTION_DEFAULTS):
            self.set_option(i, n)

    def set_param(self, k, v):
        self.m.set_param(k, v)

    def set_option(self, i, n):
        self.m.set_param(6 + i, float(n))

    def process(self, L, R):
        oL, oR = self.m.process(L, R)
        L[:] = oL; R[:] = oR

    def reset(self):
        self.m.reset()

    def dry_wet(self):
        v = self.m.params[5].get()
        return F(F(1.0) - v), v


def run(case, inputs, research, block):
    mod = research.load("lofi")
    fs = int(case["params"].get("fs", 44100))
    return effectrun.run(case, inputs, research, block, _Effect(mod, fs)), {}
