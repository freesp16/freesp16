"""Headphone-cue golden: engine::mixer::HeadphoneRouter against the research HeadphoneUnit
(`tools/fx/headphone_mix.py` HeadphoneUnit / HeadphoneRouter / Selector / VolumeKnob, `fx_processors.py` MuteSmoother,
`track_mix.py` Smoother; MIT; docs/effects/signal-flow.md "Main output and headphones", F209/F210/F213).

Per case: `{"mode": 0|1, "cue": 0..12, "level": 0..127, "profile": "reference"|"app", "events": [{"block": b,
"cue"|"mode"|"level": v}]}`, applied before block b. cue = core::model::HeadphoneCue (0 MASTER, 1..4 OUT 1-2..7-8,
5..12 mono 1..8, the research enum order 0x0d000001); level = HEADPHONES 0..127. Inputs out12.L/R .. out78.L/R (the
router's pairs; out12 is the master in M + 6CH), aux.L/R (preview + metronome) and input.L/R (input monitor); outputs
phones.L/R.

The model runs unchanged: HeadphoneUnit(source, flag98 = 1) with its 14-channel view in2/3 = out12 (CueMaster and
Cue12 both select it), in6/7 = out34, in8/9 = out56, in10/11 = out78, the other inputs silent; monitor pairs 0/1/2 =
aux, input, silence; IMonitorMixer gates (1, 1, 1) — a monitor pair is closed exactly while the CueMaster flag is set
(the app: the master already carries them, our choice); the dormant crossfade off (xfade 0, F210).
Changes, all in this adapter:
  - the VolumeKnob reads the app's gain law (core::GainLaw) instead of the reference ladder (applaw.py);
  - start-up: the selector, the gates and the knob start settled at the case's initial values;
  - an invalid cue for the mode is corrected as the app does (M + 6CH: OUT 1-2 / mono 1 / mono 2 -> MASTER; 8CH:
    MASTER -> OUT 1-2, our choice) before it reaches setHeadphoneSource (the 1.60 UI only offers valid ones, F211);
  - profile "app" (the default profile of the app): re-selecting the cue that fades out fades back in from the gain
    reached (fix of the F209 quirk), and a monitor gate follows the audible cue instead of the selected one. The
    reference profile (HeadphoneRouter::setReferenceQuirk) is the model as is.
"""
from .. import applaw
from ..f32 import bits
from .routing_router import app_fix

NAME = "routing_headphones"
SUBJECT = "routing.headphones"
RESEARCH_MODELS = ["headphone_mix", "track_mix", "fx_processors"]
OWNER_INPUTS = []
SIDES = ("L", "R")
IN_PAIRS = ("out12", "out34", "out56", "out78", "aux", "input")
VIEW = {2: 0, 3: 1, 6: 2, 7: 3, 8: 4, 9: 5, 10: 6, 11: 7}   # research HP input channel -> out12.L .. out78.R


def input_channels(case):
    return ["%s.%s" % (p, s) for p in IN_PAIRS for s in SIDES]


def output_channels(case):
    return ["phones.L", "phones.R"]


def validate(case):
    p = case["params"]
    unknown = set(p) - {"mode", "cue", "level", "profile", "events"}
    if unknown:
        raise ValueError("unknown params %s" % sorted(unknown))
    if p.get("mode", 0) not in (0, 1) or not 0 <= p.get("cue", 0) <= 12 or not 0 <= p.get("level", 127) <= 127:
        raise ValueError("mode 0|1, cue 0..12, level 0..127")
    if p.get("profile", "reference") not in ("reference", "app"):
        raise ValueError("profile reference|app")
    for ev in p.get("events", []):
        keys = set(ev) - {"block"}
        if len(keys) != 1 or not keys <= {"cue", "mode", "level"}:
            raise ValueError("unknown event %r" % ev)


def effective_cue(cue, mode):
    """engine::mixer::effectiveCue (MIX-040 lists, our correction)."""
    if mode == 0:
        return 0 if cue in (1, 5, 6) else cue
    return 1 if cue == 0 else cue


def settled_unit(hm, tm, fp, cue, level, block):
    """A HeadphoneUnit settled at its initial cue and level."""
    u = hm.HeadphoneUnit(source=cue, knob=level)
    u.router.sel.cur = u.router.sel.target
    u.knob.sm = tm.Smoother(100, tm.knob_gain(level))
    closed = 1 if u.router.master else 0                       # gates (1, 1, 1)
    u.cue_ms = [fp.MuteSmoother(u.CUE_LEN, closed) for _ in range(3)]
    return u


class Cueing:
    """The research HeadphoneUnit plus the adapter's per-profile rules; process(view14, cue6) -> [L, R]."""

    def __init__(self, research, cue, mode, level, profile, block):
        hm = research.load("headphone_mix")
        tm = research.load("track_mix")
        fp = research.load("fx_processors")
        hm.TM.VOLUME_TABLE = list(applaw.gain_table())          # the app's gain law in VolumeKnob (knob_gain)
        self.table = hm.TM.VOLUME_TABLE
        self.requested = cue; self.mode = mode; self.app = profile == "app"
        eff = effective_cue(cue, mode)
        self.unit = settled_unit(hm, tm, fp, eff, level, block)
        self.selected = eff; self.audible = eff
        router = self.unit.router
        inner = router.process

        def process(chans):                                     # the router's step inside HeadphoneUnit.process
            if self.app:
                app_fix(router.sel)
            inner(chans)
            if router.sel.cur == router.sel.target:
                self.audible = self.selected
            if self.app:                                        # the app's gate follows the audible cue
                router.master = 1 if self.audible == 0 else 0
        router.process = process

    def set_cue(self, cue=None, mode=None):
        if cue is not None:
            self.requested = cue
        if mode is not None:
            self.mode = mode
        self.selected = effective_cue(self.requested, self.mode)
        self.unit.router.set_source(self.selected)
        if self.app:
            self.unit.router.master = 1 if self.audible == 0 else 0

    def set_level(self, level):
        self.unit.knob.set(level)

    def process(self, view14, cue6):
        return self.unit.process(view14, cue6, gates=(1, 1, 1), xfade=0)

    def tables(self):
        return {"gain_law": {"source": "core/util/GainLaw.h (GainLaw::gain, index 0..127), the HEADPHONES law x 0.5",
                             "f32_bits": [bits(g) for g in self.table]}}


def view14(inputs, sl, block):
    z = [0.0] * block
    return [list(inputs[VIEW[c]][sl]) if c in VIEW else list(z) for c in range(14)]


def run(case, inputs, research, block):
    p = case["params"]
    cu = Cueing(research, p.get("cue", 0), p.get("mode", 0), p.get("level", 127), p.get("profile", "reference"), block)
    frames = len(inputs[0])
    out = [[], []]
    events = p.get("events", [])
    z = [0.0] * block
    for b in range(frames // block):
        for ev in [e for e in events if e["block"] == b]:
            if "level" in ev:
                cu.set_level(ev["level"])
            else:
                cu.set_cue(cue=ev.get("cue"), mode=ev.get("mode"))
        sl = slice(b * block, (b + 1) * block)
        cue6 = [list(inputs[8 + k][sl]) for k in range(4)] + [list(z), list(z)]
        L, R = cu.process(view14(inputs, sl, block), cue6)
        out[0].extend(L); out[1].extend(R)
    return out, cu.tables()
