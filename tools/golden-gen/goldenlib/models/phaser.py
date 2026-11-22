"""Phaser goldens (spec phaser.md FX-320…325, FX-360…362, FX-380, "Test notes"): the research model
`tools/fx/phaser.py` (class Phaser, MIT; with lofi.py, crmath.py, fdlibm_powf.py, fdlibm_tanf.py) through the research
wrapper `tools/fx/effectbase.py` (Insert wrapper state, gate mode 0, 10 Hz post high-pass on, crossfade mix: Phaser
slot 26). C++ subject: fx.mod with params.effect = "phaser" (engine::fx::Phaser in the Reference quirk profile, through
engine::fx::FxShell). Run at 44.1 and 48 kHz only: the model's tanf port fails at 22.05 kHz (H102).

The model always uses the fdlibm tanf/powf ports (the reference's libm); the C++ effect uses them only with
"parity_maths": true (FX-054; tolerance 1e-5), else std:: maths in double (tolerance 1e-4). Knobs k = the model's
slot-14 indices 0..5 (RATE, WIDTH, PHASE, FREQUENCY, FEEDBACK, MIXRATIO); option 0 = SYNC (index 6); the tempo is
index 7, every block (goldenlib/modrun.py). The knobs start at the app's defaults (core/params/catalogue/fx_phaser.cpp
= the model's INSERT_DEFAULTS, FX-362). Inputs and outputs: L, R.
"""
from .. import modrun
from ..f32 import F

NAME = "phaser"
SUBJECT = "fx.mod"
RESEARCH_MODELS = ["phaser", "lofi", "effectbase", "crmath", "fdlibm_powf", "fdlibm_tanf"]
OWNER_INPUTS = []
APP_DEFAULTS = [199.0 / 999.0, 1.0, 0.0, 1.0, 0.0, 0.0]   # = phaser.INSERT_DEFAULTS 0..5: 2 Hz, width 1, 18 kHz


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    modrun.validate(case, 1, NAME)
    fs = int(case["params"].get("fs", 44100))
    if fs not in (44100, 48000):
        raise ValueError("phaser goldens run at 44.1 or 48 kHz only (H102)")


class _Effect:
    gate = 0
    hpf = True

    def __init__(self, mod, fs):
        self.m = mod.Phaser(fs=fs)
        for k, v in enumerate(APP_DEFAULTS):
            self.m.set_param(k, F(v))

    def set_param(self, k, v):
        self.m.set_param(k, v)

    def set_option(self, i, n):
        self.m.set_param(6 + i, float(n))

    def set_bpm(self, bpm):
        self.m.set_param(7, bpm)

    def retrigger(self):
        self.m.retrigger()

    def process(self, L, R):
        oL, oR = self.m.process(L, R)
        L[:] = oL; R[:] = oR

    def reset(self):
        self.m.reset()

    def dry_wet(self):
        v = self.m.p[5].get()
        return F(F(1.0) - v), v


def run(case, inputs, research, block):
    mod = research.load("phaser")
    fs = int(case["params"].get("fs", 44100))
    return modrun.run(case, inputs, research, block, _Effect(mod, fs)), {}
