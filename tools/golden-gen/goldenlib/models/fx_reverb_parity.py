"""Reverb structural parity with the REFERENCE delay constants (spec reverb.md "Test notes" level 2): optional,
local-only, a deliberate opt-in (spec effects README "decisions to reflect elsewhere"). Never committed, never in CI:
the recipe tests/golden/reverb_parity is `local_only`, and this adapter declares a user-supplied input unless you opt
in with APP_REVERB_PARITY=1 (then gen.py writes the goldens to build/golden-local only).

At generation time the reference comb and all-pass delays are read from the research model tools/fx/send_reverb.py
(COMB_MS, AP_MS) and passed to the C++ subject in the local case file as params.constants (engine::fx::Reverb::
overrideConstants, a test-build hook); they stay in the test run. Limitation (our design): ReverbConstants describes
the decay curve by FX-822 anchors, so the reference's RT60 polynomial (with its v ≤ 0.09 blend) cannot be injected —
the model's `reverb_time` is replaced by the app curve here, as in the structural goldens (models/fx_reverb.py).
Tolerance 1e-4.
"""
import os

from . import fx_reverb

NAME = "reverb_parity"
SUBJECT = "fx.delay"
RESEARCH_MODELS = fx_reverb.RESEARCH_MODELS
OWNER_INPUTS = [] if os.environ.get("APP_REVERB_PARITY") == "1" else [
    "APP_REVERB_PARITY=1 (opt-in: the reference reverb constants, read from the model at test time)"]

input_channels = fx_reverb.input_channels
output_channels = fx_reverb.output_channels
validate = fx_reverb.validate


def run(case, inputs, research, block):
    mod = research.load("send_reverb")
    sd = research.load("send_delay")
    comb = [(float(a), float(b)) for a, b in mod.COMB_MS]
    ap = [(float(a), float(b)) for a, b in mod.AP_MS]
    fx_reverb.install_app_constants(mod, comb_ms=comb, ap_ms=ap)
    p = case["params"]
    p["constants"] = {"combL": [a for a, _ in comb], "combR": [b for _, b in comb],
                      "apL": [a for a, _ in ap], "apR": [b for _, b in ap],
                      "t60v": fx_reverb.APP_T60V, "t60s": fx_reverb.APP_T60S}
    fs = int(p.get("fs", 44100))
    eff = fx_reverb._Effect(mod, sd, fs, p.get("slot", "send") == "master")
    out = fx_reverb.delayrun.run(case, inputs, research, block, eff)
    if eff.oob():
        raise ValueError("%s: the model read past a ring end (FX-882): choose other settings" % case["id"])
    return out, {}
