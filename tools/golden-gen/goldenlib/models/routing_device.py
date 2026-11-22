"""Output-stage golden: engine::mixer::OutputRouterStage steps 12-13 (headphone cue, output trim,
mute-all) against the research device output stage (`tools/fx/device_out.py` DeviceOut: OutputVolume per pair,
FixedAttenuator, BulkMute; `tools/fx/headphone_mix.py` HeadphoneUnit; MIT; docs/effects/signal-flow.md "Device output
stage", F109-F113, and "Main output and headphones").

Per case: `{"trim": 0..1023, "phones_level": 0..127, "cue": 0..12, "events": [{"block": b, "trim"|"mute_all"|
"phones_level"|"cue": v}]}`, applied before block b; M + 6CH, no track assigned, the Main and Phones ports present.
Inputs master.L/R (post master fader) and aux.L/R (preview + metronome); outputs main.L/R and phones.L/R (the device
pairs 0 and 4).

Used from device_out.py: the pair layout, OutputVolume (100-sample GainSmoother,
the same object per pair) and BulkMute (64-sample MuteSmoother per pair, all pairs adopt the target together), with
  - the FixedAttenuator at cfg = 1.0 and no sign flip (the x(-0.5) DAC scaling is not reproduced, MIX-060, our choice);
  - the OutputVolume gain = the app's own output-trim table (engine::mixer::outputTrimGain: 0 = mute, 1 = -60 dB,
    linear in dB to 1023 = 0 dB), plugged in by replacing device_out.outvol_gain: device_out.load_table (a firmware
    table) is never called (research.py stubs it);
  - every OutputVolume and the headphone unit start settled at the case's values; BulkMute starts unmuted, as
    the reference after its constructor.
The headphones are routing_headphones.Cueing (reference profile) with in2/3 = the master (OUT 1-2 in M + 6CH).
Not reproduced (the cases avoid it): BulkMute's re-toggle quirk — a change back within the block after a fade, while
the per-pair smoothers have not reported "done", is adopted without a fade by the reference; the app always fades.
"""
import math

from ..f32 import F, bits
from .routing_headphones import Cueing

NAME = "routing_device"
SUBJECT = "routing.device"
RESEARCH_MODELS = ["device_out", "headphone_mix", "track_mix", "fx_processors"]
OWNER_INPUTS = []
SIDES = ("L", "R")


def input_channels(case):
    return ["master.L", "master.R", "aux.L", "aux.R"]


def output_channels(case):
    return ["main.L", "main.R", "phones.L", "phones.R"]


def validate(case):
    p = case["params"]
    unknown = set(p) - {"trim", "phones_level", "cue", "events"}
    if unknown:
        raise ValueError("unknown params %s" % sorted(unknown))
    if not 0 <= p.get("trim", 1023) <= 1023 or not 0 <= p.get("phones_level", 127) <= 127:
        raise ValueError("trim 0..1023, phones_level 0..127")
    for ev in p.get("events", []):
        keys = set(ev) - {"block"}
        if len(keys) != 1 or not keys <= {"trim", "mute_all", "phones_level", "cue"}:
            raise ValueError("unknown event %r" % ev)


def trim_table():
    """engine::mixer::outputTrimGain (src/engine/mixer/Mixer.cpp): float(pow(10, (-60 + (i - 1) * 60/1022) / 20))."""
    return [0.0] + [F(math.pow(10.0, (-60.0 + (i - 1) * (60.0 / 1022.0)) / 20.0)) for i in range(1, 1024)]


def run(case, inputs, research, block):
    dm = research.load("device_out")
    tm = research.load("track_mix")
    table = trim_table()
    dm.outvol_gain = lambda index, t=None: table[index]     # the app's trim table (OutputVolume.apply reads the global)
    p = case["params"]
    trim = p.get("trim", 1023)
    dev = dm.DeviceOut(cfg=[1.0] * dm.PAIRS, flags=[0] * dm.PAIRS, indices=[trim] * dm.PAIRS)
    for v in dev.vol:
        v.sm = tm.Smoother(dm.RAMP, table[trim])
    cu = Cueing(research, p.get("cue", 0), 0, p.get("phones_level", 127), "reference", block)
    frames = len(inputs[0])
    out = [[] for _ in range(4)]
    z = [0.0] * block
    for b in range(frames // block):
        for ev in [e for e in case["params"].get("events", []) if e["block"] == b]:
            if "trim" in ev:
                for v in dev.vol:
                    v.set(ev["trim"])
            elif "mute_all" in ev:
                dev.bulk.set_muted(bool(ev["mute_all"]))
            elif "phones_level" in ev:
                cu.set_level(ev["phones_level"])
            else:
                cu.set_cue(cue=ev["cue"])
        sl = slice(b * block, (b + 1) * block)
        view = [list(z) for _ in range(14)]
        view[2] = list(inputs[0][sl]); view[3] = list(inputs[1][sl])
        hl, hr = cu.process(view, [list(inputs[2][sl]), list(inputs[3][sl])] + [list(z) for _ in range(4)])
        chans = [list(z) for _ in range(14)]
        chans[0] = list(inputs[0][sl]); chans[1] = list(inputs[1][sl])
        chans[8] = hl; chans[9] = hr
        res = dev.process(chans)
        for c, k in zip(out, (0, 1, 8, 9)):
            c.extend(res[k])
    tables = cu.tables()
    tables["output_trim"] = {"source": "src/engine/mixer/Mixer.cpp (outputTrimGain, index 0..1023)",
                             "f32_bits": [bits(g) for g in table]}
    return out, tables
