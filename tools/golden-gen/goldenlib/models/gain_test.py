"""GainTest golden: an own float32 reference of tests/golden/harness/GainTest.h (no research model; GPL-3.0-or-later).

GainTest is the trivial engine::fx::EffectBase that proves the harness end to end:
  parameter 0 = gain index: value01 -> index = int(value01 * 127 + 0.5) (float32), clamped 0..127 -> GainLaw::gain(index);
  default index 100 (unity). A new target is reached by a linear ramp over one 64-frame block:
    step = (target - cur) * (1/64); per frame: cur += step; L *= cur; R *= cur; after the block cur = target.
  With no change pending: L *= cur; R *= cur.
Case params: {"index": k (initial, applied before the first block, no ramp), "events": [{"block": b, "index": k}]}.
An event's value01 is float32(k / 127.0) (double division, then rounded), as the C++ subject passes it.
Inputs and outputs: L, R.
"""
from .. import applaw
from ..f32 import F, bits

NAME = "gain_test"
SUBJECT = "fx.gain_test"
RESEARCH_MODELS = []
OWNER_INPUTS = []


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"]


def validate(case):
    for ev in case["params"].get("events", []):
        if set(ev) != {"block", "index"}:
            raise ValueError("gain_test events are {block, index}: %r" % ev)


def _index(value01):
    i = int(F(F(value01 * 127.0) + 0.5))
    return 0 if i < 0 else (127 if i > 127 else i)


def run(case, inputs, research, block):
    table = applaw.gain_table()
    p = case["params"]
    cur = target = table[_index(F(p.get("index", 100) / 127.0))]
    inv = F(1.0 / 64.0)
    L, R = inputs
    oL, oR = [], []
    events = p.get("events", [])
    for b in range(len(L) // block):
        for ev in events:
            if ev["block"] == b:
                target = table[_index(F(ev["index"] / 127.0))]
        xl = L[b * block:(b + 1) * block]; xr = R[b * block:(b + 1) * block]
        if cur != target:
            step = F(F(target - cur) * inv)
            for i in range(block):
                cur = F(cur + step)
                oL.append(F(xl[i] * cur)); oR.append(F(xr[i] * cur))
            cur = target
        else:
            oL.extend(F(x * cur) for x in xl); oR.extend(F(x * cur) for x in xr)
    tables = {"gain_law": {"source": "core/util/GainLaw.h (GainLaw::gain, index 0..127)",
                           "f32_bits": [bits(g) for g in table]}}
    return [oL, oR], tables
