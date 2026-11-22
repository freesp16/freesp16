"""Shared modulation oscillator goldens (spec oscillator.md FX-1300…1308, "Test notes"): the research
model `tools/fx/osc.py` (class Osc, MIT; with crmath.py and fdlibm_sincosf.py) driving the app's own random table
(FX-1305) through its `table=` argument. No firmware table is ever loaded. C++ subject: fx.oscillator
(tests/golden/harness/OscillatorSubject.cpp: engine::fx::ModOscillator, src/engine/fx/effects/Oscillator.h).

The table (FX-1305, our choice): 8192 float32 values, xorshift32 from seed 0x5EED1600 (x ^= x << 13; x ^= x >> 17;
x ^= x << 5; the state advances before each value), value = (x >> 8) * 2^-23 - 1. osc.py expects the table plus the
following word (its index wrap reaches index 0x2000): the app passes table[0] there (our choice).

params: {"wave": 0..6, "freq": Hz, "fs": Hz (optional, default the recipe's 44100), "amp": a (1), "offset": o (0),
         "phase": phi (R phase offset, optional), "smoothing_hz": f (optional), "smoothing_off": bool (optional),
         "rand_offset": k (optional), "reset_phase": phi (optional, after the set-up),
         "stride": S (1; each golden block renders S oscillator blocks of 64 samples and keeps every S-th sample,
                      the last of each group: frame j of golden block b = sample 64*S*b + S*j + S-1),
         "events": [{"block": b, "op": "set_frequency"|"set_period_ms"|"set_phase_offset"|"reset_phase"|
                     "set_smoothing_hz"|"set_amplitude"|"set_offset"|"hold"|"clear_hold"|"smoothing_off", "v": x}]}
Set-up order (the subject does the same): set_rand_offset -> init (ctor) -> set_amplitude -> set_offset ->
set_phase_offset -> set_smoothing_hz -> set_smoothing_off -> reset_phase. Events apply before golden block b.
No inputs; outputs L, R.
"""
import struct

from ..f32 import F

NAME = "oscillator"
SUBJECT = "fx.oscillator"
RESEARCH_MODELS = ["osc", "crmath", "fdlibm_sincosf"]
OWNER_INPUTS = []

SEED = 0x5EED1600
TABLE_SIZE = 8192
OPS = {"set_frequency", "set_period_ms", "set_phase_offset", "reset_phase", "set_smoothing_hz", "set_amplitude",
       "set_offset", "hold", "clear_hold", "smoothing_off"}


def random_table():
    """FX-1305: the app's own table (float32 values as Python floats)."""
    x = SEED
    out = []
    for _ in range(TABLE_SIZE):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(F((x >> 8) * 2.0 ** -23 - 1.0))
    return out


def fnv1a32(values):
    h = 0x811C9DC5
    for v in values:
        for b in struct.pack("<f", v):
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def input_channels(case):
    return []


def output_channels(case):
    return ["L", "R"]


def validate(case):
    p = case["params"]
    if int(p.get("wave", 0)) not in range(7):
        raise ValueError("oscillator wave must be 0..6: %r" % p.get("wave"))
    s = int(p.get("stride", 1))
    if s < 1 or 64 % s:
        raise ValueError("oscillator stride must divide 64: %r" % s)
    for ev in p.get("events", []):
        if ev.get("op") not in OPS or not set(ev) <= {"block", "op", "v"}:
            raise ValueError("oscillator events are {block, op, v}: %r" % ev)


def _apply(o, op, v):
    if op == "set_frequency": o.set_frequency(F(v))
    elif op == "set_period_ms": o.set_period_ms(F(v))
    elif op == "set_phase_offset": o.set_phase_offset(F(v))
    elif op == "reset_phase": o.reset_phase(F(v))
    elif op == "set_smoothing_hz": o.set_smoothing_hz(F(v))
    elif op == "set_amplitude": o.set_amplitude(F(v))
    elif op == "set_offset": o.set_offset(F(v))
    elif op == "hold": o.set_hold()
    elif op == "clear_hold": o.clear_hold()
    elif op == "smoothing_off": o.set_smoothing_off(1 if v else 0)


def run(case, inputs, research, block):
    osc = research.load("osc")
    p = case["params"]
    table = random_table()
    fs = int(p.get("fs", 44100))
    o, _ = osc.Osc.ctor(int(p.get("wave", 0)), F(p.get("freq", 1.0)), fs, 1, table=table + [table[0]],
                        rand_off=int(p.get("rand_offset", 0)))
    o.set_amplitude(F(p.get("amp", 1.0)))
    o.set_offset(F(p.get("offset", 0.0)))
    if "phase" in p:
        o.set_phase_offset(F(p["phase"]))
    if "smoothing_hz" in p:
        o.set_smoothing_hz(F(p["smoothing_hz"]))
    if p.get("smoothing_off"):
        o.set_smoothing_off(1)
    if "reset_phase" in p:
        o.reset_phase(F(p["reset_phase"]))
    stride = int(p.get("stride", 1))
    blocks = int(case["blocks"])
    oL, oR = [], []
    for b in range(blocks):
        for ev in p.get("events", []):
            if ev["block"] == b:
                _apply(o, ev["op"], ev.get("v", 0.0))
        gl, gr = [], []
        for _ in range(stride):
            res = o.process(block)
            if res is None:
                raise ValueError("oscillator: invalid set-up (the model returned no output)")
            gl += res[0]; gr += res[1]
        oL += gl[stride - 1::stride]; oR += gr[stride - 1::stride]
    tables = {"osc_random": {"source": "engine/fx/effects/OscillatorRandomTable.h (FX-1305: xorshift32, seed "
                                       "0x5EED1600, (x >> 8) * 2^-23 - 1); 8193rd word = table[0]",
                             "count": TABLE_SIZE, "fnv1a32": fnv1a32(table)}}
    return [oL, oR], tables
