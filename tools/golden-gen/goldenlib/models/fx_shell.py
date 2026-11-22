"""FX shell and processor goldens (spec effect-shell.md "Test notes"): the research wrapper
`tools/fx/effectbase.py` (EffectBase: slot 5/6/7/18, InOutGain, IirHpf1stOrder, SignalGate) and the hosts
`tools/fx/fx_processors.py` (MasterProcessor, SendProcessor, InsertProcessorFull, MuteSmoother) — MIT — driving own
stub effects. C++ subject: fx.shell (tests/golden/harness/FxShellSubject.cpp: engine::fx::FxShell / FxProcessor).

params.kind = "shell": one EffectBase around the stub "wet = 0.5 * input" (spec test note).
  {"gate": 0|1, "hpf": bool, "law": "crossfadeK5"|"unityK5"|"wetOnly"|"delay"|"reverb", "mix": v (k5, initial),
   "skip": bool (reference skip/clamp, the mid-block activation and stop: Insert Compressor quirk FX-033),
   "events": [{"block": b, "op": "activate"|"stop"|"mix", "v": v}]}   applied before block b
  activate = slot5(0.0, 1) + slot7(); stop = slot6(1.0); mix = the law's dry/wet for v (slot 26 values).
params.kind = "insert"|"send"|"master": a processor over 10 stub types; stub i: wet = g_i * input, g_i = f32((i+1)/16)
  (gate 0, no HPF, slot 26: dry 0 / wet 1, slot22(0)); with "tempo_stub": the processor's tempo-fed indices use
  g = f32(bpm) * 2^-10 instead (the clamped feed made audible).
  {"events": [{"block": b, "type": t (-1 = Off)} | {"block": b, "bypass": 0|1} | {"block": b, "disable": 0|1} |
              {"block": b, "bpm": x}], "bpm": x0, "tempo_stub": bool}
  Send starts with its mute open (reference start-up, FX-048).
Every case repeats the recipe's sample rate as params.fs. Inputs: L, R. Outputs: L, R, and for processors "current" (the current type index after each block, per frame).
"""
from ..f32 import F

NAME = "fx_shell"
SUBJECT = "fx.shell"
RESEARCH_MODELS = ["effectbase", "fx_processors"]
OWNER_INPUTS = []
FEED = {"insert": (0, 1, 4, 8), "send": (0, 2, 3, 9), "master": (0, 2, 3, 9)}


def input_channels(case):
    return ["L", "R"]


def output_channels(case):
    return ["L", "R"] if case["params"].get("kind", "shell") == "shell" else ["L", "R", "current"]


def validate(case):
    p = case["params"]
    kind = p.get("kind", "shell")
    if kind not in ("shell", "insert", "send", "master"):
        raise ValueError("fx_shell kind: %r" % kind)
    for ev in p.get("events", []):
        keys = set(ev) - {"block"}
        if kind == "shell":
            if ev.get("op") not in ("activate", "stop", "mix") or not keys <= {"op", "v"}:
                raise ValueError("shell events are {block, op, v}: %r" % ev)
        elif len(keys) != 1 or not keys <= {"type", "bypass", "disable", "bpm"}:
            raise ValueError("processor events are {block, type|bypass|disable|bpm}: %r" % ev)


def law(name, v):
    """The FX-030 dry/wet laws, float32 as the research models (insert_delay/send_reverb update_mix)."""
    v = F(v)
    if name == "wetOnly":
        return F(0.0), F(1.0)
    if name == "unityK5":
        return F(1.0), v
    if name == "crossfadeK5":
        return F(F(1.0) - v), v
    if name in ("delay", "reverb"):
        if v < F(0.75):
            d, w = F(1.0), F(v * F(1.3333334))
        else:
            w, d = F(1.0), F(F(1.0) + F(F(v - F(0.75)) * F(-4.0)))
        if name == "reverb":
            w = F(w * F(0.70794576))
        return d, w
    raise ValueError("unknown law %r" % name)


def _run_shell(p, inputs, eb_mod, block, fs):
    state = {"v": F(p.get("mix", 0.0))}
    name = p.get("law", "crossfadeK5")

    def s25(eb, L, R, n):
        for i in range(n):
            L[i] = F(L[i] * 0.5); R[i] = F(R[i] * 0.5)
        return 0

    def s26(eb):
        eb.dry, eb.wet = law(name, state["v"])

    eb = eb_mod.EffectBase(fs=int(fs), block=block, mode=int(p.get("gate", 0)), slot25=s25, slot26=s26)
    eb.slot22(1 if p.get("skip") else 0)
    eb.set_post_hpf(bool(p.get("hpf")), 10.0)
    L, R = inputs
    oL, oR = list(L), list(R)
    for b in range(len(L) // block):
        for ev in p.get("events", []):
            if ev["block"] != b:
                continue
            if ev["op"] == "activate":
                eb.slot5(0.0, 1); eb.slot7()
            elif ev["op"] == "stop":
                eb.slot6(1.0)
            else:
                state["v"] = F(ev["v"]); s26(eb)
        bl = oL[b * block:(b + 1) * block]; br = oR[b * block:(b + 1) * block]
        eb.slot18(bl, br, block)
        oL[b * block:(b + 1) * block] = bl; oR[b * block:(b + 1) * block] = br
    return [oL, oR]


def _run_processor(p, inputs, eb_mod, fp, block, fs):
    kind = p["kind"]
    tempo = {"bpm": float(p.get("bpm", 120.0))}
    tempo_stub = bool(p.get("tempo_stub"))
    gains = {}

    def stub(i):
        def s25(eb, L, R, n):
            g = gains.get(i, F((i + 1) / 16.0))
            for k in range(n):
                L[k] = F(L[k] * g); R[k] = F(R[k] * g)
            return 0

        def s26(eb):
            eb.dry, eb.wet = F(0.0), F(1.0)
        eb = eb_mod.EffectBase(fs=int(fs), block=block, mode=0, slot25=s25, slot26=s26)
        eb.slot22(0)
        return eb

    def feed(idx, eb, bpm):
        if tempo_stub:
            gains[idx] = F(F(bpm) * F(1.0 / 1024.0))

    effects = {i: stub(i) for i in range(10)}
    common = dict(count=10, tempo=lambda: tempo["bpm"], feed=feed)
    if kind == "master":
        proc = fp.MasterProcessor(effects, **common)
    elif kind == "send":
        proc = fp.SendProcessor(effects, fs=int(fs), **common)
    else:
        proc = fp.InsertProcessorFull(effects, fs=int(fs), **common)
    L, R = inputs
    oL, oR, cur = list(L), list(R), []
    for b in range(len(L) // block):
        for ev in p.get("events", []):
            if ev["block"] != b:
                continue
            if "type" in ev:
                proc.set_fx_type(int(ev["type"]))
            elif "bypass" in ev:
                proc.set_bypass(int(ev["bypass"]))
            elif "disable" in ev:
                proc.set_disabled(int(ev["disable"]))
            else:
                tempo["bpm"] = float(ev["bpm"])
        bl = oL[b * block:(b + 1) * block]; br = oR[b * block:(b + 1) * block]
        proc.process_block(bl, br, block)
        oL[b * block:(b + 1) * block] = bl; oR[b * block:(b + 1) * block] = br
        cur.extend([F(float(proc.cur))] * block)
    return [oL, oR, cur]


def run(case, inputs, research, block):
    eb_mod = research.load("effectbase")
    fp = research.load("fx_processors")
    p = case["params"]
    fs = float(p.get("fs", 44100))  # = the recipe's sample_rate (the C++ subject checks)
    if p.get("kind", "shell") == "shell":
        return _run_shell(p, inputs, eb_mod, block, fs), {}
    return _run_processor(p, inputs, eb_mod, fp, block, fs), {}
