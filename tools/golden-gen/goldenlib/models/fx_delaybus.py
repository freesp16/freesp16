"""Send/Master Delay goldens (spec send-delay.md FX-720…725, FX-760/761, "Test notes"): the research
model `tools/fx/send_delay.py` (class SendDelay, MIT; with insert_delay.py, lofi.py, chorus.py) through the research
wrapper `tools/fx/effectbase.py` (gate mode 1, no post high-pass, the piecewise delay mix law of k5), the tempo fed as
parameter 7 every block. C++ subject: fx.delay with params.effect = "delayBus" in the Send or Master slot.

Knobs k = the model's indices 0..5 (DelayTime, Feedback, HiCut, LowCut, Spread, Mixratio); option 0 = SYNC (index 6).
The knobs start at the app's defaults of the slot (core/params/catalogue/fx_delayBus.cpp = the model's SEND_DEFAULTS /
MASTER_DEFAULTS: only Mixratio differs). Shared case parameters: goldenlib/delayrun.py ("slot": "send" | "master").
"""
from .. import delayrun
from ..f32 import F
from .fx_shell import law

NAME = "delayBus"
SUBJECT = "fx.delay"
RESEARCH_MODELS = ["send_delay", "insert_delay", "effectbase", "lofi", "chorus"]
OWNER_INPUTS = []
SEND_DEFAULTS = [490.0 / 3990.0, 0.7, 1.0, 0.0, 0.0, 1.0]
MASTER_DEFAULTS = [490.0 / 3990.0, 0.7, 1.0, 0.0, 0.0, 0.5]


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    if case["params"].get("effect", "delayBus") != "delayBus":
        raise ValueError("delayBus recipe: params.effect must be delayBus")
    delayrun.validate(case, 1, slots=("send", "master"))


class _Effect:
    gate = 1
    hpf = False

    def __init__(self, mod, fs, master):
        self.m = mod.SendDelay(fs=fs)
        for k, v in enumerate(MASTER_DEFAULTS if master else SEND_DEFAULTS):
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
    mod = research.load("send_delay")
    p = case["params"]
    fs = int(p.get("fs", 44100))
    return delayrun.run(case, inputs, research, block, _Effect(mod, fs, p.get("slot", "send") == "master")), {}
