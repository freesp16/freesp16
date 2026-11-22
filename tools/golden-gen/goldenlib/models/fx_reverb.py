"""Reverb structural goldens (spec reverb.md FX-820…828, FX-860…862, "Test notes"). The app's reverb keeps
the reference structure and laws with its OWN constants (FX-821 comb delays, FX-822 decay curve, FX-827 all-pass
delays), so it is never compared with the reference's firmware values. This adapter plugs the app's constants into
the research model `tools/fx/send_reverb.py` (class SendReverb, MIT; with insert_delay.py, send_delay.py, lofi.py,
fdlibm_powf.py): module attributes COMB_MS and AP_MS are replaced by the app's delays and `reverb_time` by the app's
FX-822 curve (below, = engine::fx::Reverb::t60), and at rates other than 44.1 kHz the damping filters get the app's
maximum min(18 kHz, 0.4·fs) (FX-824). The model then runs through `tools/fx/effectbase.py` (gate mode 1, no post
high-pass, the reverb mix law). No firmware-derived value enters these goldens: the model's own reference constants
are overwritten before the first use. C++ subject: fx.delay with params.effect = "reverb" in the Reference profile
(every FX-88x quirk on, as the model: warm-up gates, stale rings, shared ramp counters, 100 ms first glide).

Tolerance 1e-4 (the parity tolerance): the comb gains are 10^(−3d/(1000T)) by the model's fdlibm powf here and
by double pow in the app (≤ 1 ulp). The model's one-past-end reads (FX-882, fixed in the app) must not occur in a case:
the adapter fails if any is counted. Knobs k = the model's indices 0..5; no options; no tempo. In/out: L, R.
"""
import math

from .. import delayrun
from ..f32 import F
from .fx_shell import law

NAME = "reverb"
SUBJECT = "fx.delay"
RESEARCH_MODELS = ["send_reverb", "send_delay", "insert_delay", "effectbase", "lofi", "chorus", "fdlibm_powf"]
OWNER_INPUTS = []
# engine/fx/effects/Reverb.h ReverbConstants (the app's own design, FX-821/822/827)
APP_COMB_MS = [(22.63, 38.29), (39.47, 23.71), (24.37, 40.53), (41.13, 25.19), (21.89, 24.43)]
APP_AP_MS = [(7.03, 8.19), (22.81, 21.37)]
APP_T60V = [0.0, 0.09, 0.3, 49.0 / 99.0, 1.0]
APP_T60S = [0.10, 0.87, 3.55, 8.70, 100.6]
SEND_DEFAULTS = [0.0, 49.0 / 99.0, 49.0 / 99.0, 0.0, 0.0, 1.0]     # core/params/catalogue/fx_reverb.cpp (FX-092/093)
MASTER_DEFAULTS = [0.0, 49.0 / 99.0, 49.0 / 99.0, 0.0, 0.0, 0.5]


def app_t60(v):
    """FX-822 as engine::fx::Reverb::t60: log T60 linear in v between the anchors (double maths, float32 result)."""
    tv = [F(x) for x in APP_T60V]; ts = [F(x) for x in APP_T60S]
    v = F(v)
    x = 0.0 if v < 0.0 else (1.0 if v > F(1.0) else float(v))
    i = 0
    while i < 3 and x > tv[i + 1]:
        i += 1
    l0 = math.log(ts[i]); l1 = math.log(ts[i + 1])
    u = (x - tv[i]) / (tv[i + 1] - tv[i]) if tv[i + 1] > tv[i] else 0.0
    return F(math.exp(l0 + u * (l1 - l0)))


def app_damping_max(fs):
    return 18000.0 if fs == 44100 else min(18000.0, 0.4 * fs)


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    if case["params"].get("effect", "reverb") != "reverb":
        raise ValueError("reverb recipe: params.effect must be reverb")
    delayrun.validate(case, 0, slots=("send", "master"))


def install_app_constants(mod, comb_ms=None, ap_ms=None, t60=None):
    mod.COMB_MS = [(F(a), F(b)) for a, b in (comb_ms or APP_COMB_MS)]
    mod.AP_MS = [(F(a), F(b)) for a, b in (ap_ms or APP_AP_MS)]
    mod.reverb_time = t60 or app_t60


class _Effect:
    gate = 1
    hpf = False

    def __init__(self, mod, sd_mod, fs, master):
        self.m = mod.SendReverb(fs=fs)
        if fs != 44100:
            for c in self.m.combs:
                c.lpf = sd_mod.SteppedFilter("lpf", fs, 100, 2000, app_damping_max(fs))
        for k, v in enumerate(MASTER_DEFAULTS if master else SEND_DEFAULTS):
            self.m.set_param(k, F(v))

    def set_param(self, k, v):
        self.m.set_param(k, v)

    def set_option(self, i, n):
        raise ValueError("the reverb has no options")

    def set_bpm(self, bpm):
        pass                                            # no tempo feed for the reverb (F123)

    def process(self, L, R):
        oL, oR = self.m.process(L, R)
        L[:] = oL; R[:] = oR

    def reset(self):
        self.m.reset()

    def dry_wet(self):
        return law("reverb", self.m.p[5].get())

    def oob(self):
        n = self.m.pd.oob
        for c in self.m.combs:
            n += c.oob
        for a in self.m.aps:
            n += a.oob
        return n


def run(case, inputs, research, block):
    mod = research.load("send_reverb")
    sd = research.load("send_delay")
    install_app_constants(mod)
    p = case["params"]
    fs = int(p.get("fs", 44100))
    eff = _Effect(mod, sd, fs, p.get("slot", "send") == "master")
    out = delayrun.run(case, inputs, research, block, eff)
    if eff.oob():
        raise ValueError("%s: the model read past a ring end %d times (FX-882, fixed in the app): choose other "
                         "settings" % (case["id"], eff.oob()))
    return out, {}
