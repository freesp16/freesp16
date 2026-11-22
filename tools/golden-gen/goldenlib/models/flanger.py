"""Flanger goldens (spec flanger.md FX-220…224, FX-260…263, FX-280…282, "Test notes"): the research
model `tools/fx/flanger.py` (classes Flanger/Kernel, MIT; with chorus.py, osc.py, crmath.py, fdlibm_sincosf.py) through
the research wrapper `tools/fx/effectbase.py` (Insert wrapper state, gate mode 0, 10 Hz post high-pass on, unity-dry
mix: Flanger slot 26). C++ subject: fx.mod with params.effect = "flanger" (engine::fx::Flanger in the Reference quirk
profile, through engine::fx::FxShell).

Reference profile: the kernel is built at 44,100 Hz whatever the engine rate (FX-280), so the model is constructed with
fs = 44100. Knobs k = the model's slot-14 indices 0..5 (RATE, DEPTH = width, PHASE, DEPTH SCALE = delay, FEEDBACK,
MIXRATIO); option 0 = SYNC (index 6); the tempo is index 7, every block (goldenlib/modrun.py). The knobs start at the
app's defaults (core/params/catalogue/fx_flanger.cpp, FX-263). Inputs and outputs: L, R.
"""
from .. import modrun
from ..f32 import F

NAME = "flanger"
SUBJECT = "fx.mod"
RESEARCH_MODELS = ["flanger", "chorus", "osc", "effectbase", "crmath", "fdlibm_sincosf"]
OWNER_INPUTS = []
APP_DEFAULTS = [19.0 / 99.0, 1.0, 0.0, 0.0, 0.0, 0.0]   # rate 0.20 Hz, width 100, phase 0, delay x0.1, fb 0, mix 0


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    modrun.validate(case, 1, NAME)
    if case["params"].get("parity_maths"):
        raise ValueError("flanger: parity_maths is a Phaser option")


class _Effect:
    gate = 0
    hpf = True

    def __init__(self, mod):
        self.m = mod.Flanger(fs=44100)         # FX-280: the reference kernel is always built at 44.1 kHz
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
        self.m.process(L, R)                   # in place

    def reset(self):
        self.m.reset()

    def dry_wet(self):
        return F(1.0), self.m.mix


def run(case, inputs, research, block):
    mod = research.load("flanger")
    return modrun.run(case, inputs, research, block, _Effect(mod)), {}
