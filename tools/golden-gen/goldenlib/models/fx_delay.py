"""Insert Delay goldens (spec insert-delay.md FX-620…625, FX-660/661, "Test notes"): the research model
`tools/fx/insert_delay.py` (class InsertDelay, MIT; with lofi.py, chorus.py) through the research wrapper
`tools/fx/effectbase.py` (gate mode 1, no post high-pass, the piecewise delay mix law of k5), the tempo fed as parameter
7 every block. C++ subject: fx.delay with params.effect = "delay" (engine::fx::InsertDelay in the Reference quirk
profile, through engine::fx::FxShell).

Knobs k = the model's indices 0..5 (DelayTime, Feedback, HiCut, LowCut, Spread, Mixratio); option 0 = SYNC (model
index 6). The knobs start at the app's Insert defaults (core/params/catalogue/fx_delay.cpp, = the model's
INSERT_DEFAULTS; k3/k4 have no ParamSpec and stay 0). Shared case parameters: goldenlib/delayrun.py. In/out: L, R.
"""
from .. import delayrun
from ..f32 import F
from .fx_shell import law

NAME = "delay"
SUBJECT = "fx.delay"
RESEARCH_MODELS = ["insert_delay", "effectbase", "lofi", "chorus"]
OWNER_INPUTS = []
APP_DEFAULTS = [49.0 / 99.0, 0.7, 1.0, 0.0, 0.0, 0.0]   # FX-092 = insert_delay.INSERT_DEFAULTS (bits 0x3efd6a05, ...)


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    if case["params"].get("effect", "delay") != "delay":
        raise ValueError("delay recipe: params.effect must be delay")
    delayrun.validate(case, 1, slots=("insert",))


class _Effect:
    gate = 1
    hpf = False

    def __init__(self, mod, fs):
        self.m = mod.InsertDelay(fs=fs)
        for k, v in enumerate(APP_DEFAULTS):
            self.m.set_param(k, F(v))

    def set_param(self, k, v):
        self.m.set_param(k, v)

    def set_option(self, i, n):
        self.m.set_param(6, float(n))

    def set_bpm(self, bpm):
        self.m.set_param(7, bpm)

    def process(self, L, R):
        oL, oR = self.m.process(L, R)
        L[:] = oL; R[:] = oR

    def reset(self):
        self.m.reset()

    def dry_wet(self):
        return law("delay", self.m.p[5].get())


def run(case, inputs, research, block):
    mod = research.load("insert_delay")
    fs = int(case["params"].get("fs", 44100))
    return delayrun.run(case, inputs, research, block, _Effect(mod, fs)), {}
