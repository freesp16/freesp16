"""Distortion goldens (spec distortion.md FX-1020…1024, FX-1060, "Test notes"): the research model
`tools/fx/distortion.py` (classes Kernel, Distortion, MIT; with osc.py, crmath.py, fdlibm_powf.py) through the research
wrapper `tools/fx/effectbase.py` (Insert wrapper state, gate mode 0, 10 Hz post high-pass on, crossfade mix). C++
subject: fx.effect with params.effect = "distortion" (engine::fx::Distortion in the Reference quirk profile, through
engine::fx::FxShell).

distortion.py imports `glibc_expf.py` (LGPL-2.1-or-later, an own re-implementation of the glibc 2.13 expf algorithm).
golden-gen never loads its tables (research.py stubs `load_tables`), so expf falls back to the correctly rounded
crmath.expf — the CI golden; the bit-exact variant with the user's own tables is
local-only and never committed. Nothing of it is linked into the app.

Knobs k = the model's set_param indices: 0 drive, 1 tone (output LPF), 2 lo freq (input HPF), 3 hi freq (pre-clip
LPF), 4 dynamics, 5 mix; the app's defaults (core/params/catalogue/fx_distortion.cpp) first. No options. Shared case
parameters: goldenlib/effectrun.py. Inputs and outputs: L, R.
"""
from .. import effectrun
from ..f32 import F

NAME = "distortion"
SUBJECT = "fx.effect"
RESEARCH_MODELS = ["distortion", "osc", "glibc_expf", "fdlibm_powf", "crmath", "fdlibm_sincosf", "effectbase", "lofi"]
OWNER_INPUTS = []
APP_DEFAULTS = [0.0, 1.0, 0.0, 1.0, 0.0, 1.0]   # spec distortion.md parameter table (k order)


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    if case["params"].get("effect", "distortion") != "distortion":
        raise ValueError("distortion recipe: params.effect must be distortion")
    effectrun.validate(case, 0)


class _Effect:
    gate = 0
    hpf = True

    def __init__(self, mod, fs):
        self.m = mod.Distortion(fs=fs)
        for k, v in enumerate(APP_DEFAULTS):
            self.m.set_param(k, F(v))

    def set_param(self, k, v):
        self.m.set_param(k, v)

    def set_option(self, i, n):
        raise ValueError("distortion has no options")

    def process(self, L, R):
        self.m.process(L, R)

    def reset(self):
        self.m.reset()

    def dry_wet(self):
        return self.m.dry, self.m.wet


def run(case, inputs, research, block):
    mod = research.load("distortion")
    fs = int(case["params"].get("fs", 44100))
    return effectrun.run(case, inputs, research, block, _Effect(mod, fs)), {}
