"""Output-router golden: engine::mixer::OutputRouter against the research Selector
(`tools/fx/headphone_mix.py` Selector / Output12Router, MIT; docs/effects/signal-flow.md "Main output and headphones",
F209/F212).

Per case: `{"mode": 0|1, "assign": [OutputAssign index per track], "present": pair mask, "profile": "reference"|"app",
"events": [{"block": b, "mode": m} | {"block": b, "track": t, "assign": i} | {"block": b, "present": mask}]}`, applied
before block b (the engine's block-start drain). mode 0 = M + 6CH, 1 = 8CH; assign = core::model::OutputAssign (0 "--",
1..4 OUT 1-2..7-8, 5..12 mono 1..8); present bit p = OUT pair p exists (bit 0 always). Inputs master.L/R (the master,
post master fader) and t<k>.L/R (track k's post-fader feed); outputs out12.L/R .. out78.L/R (zeros where the app
leaves an output silent).

What comes from the research model, unchanged: every fade is a headphone_mix.Selector (128-sample GainSmoother,
fade the old selection out, switch in the first block that starts with the fade-out over, fade the new one in; a
change during a fade-in fades out over 128 samples from the gain reached; F209 quirk: re-selecting the selection that
fades out leaves the gain at 0 until the next change). The main-output selector of a mode change is the reference
Output12Router (master vs the 8CH OUT 1-2 bus, F212).

What this adapter adds, all app rules (our choice, OutputRouter.h), not in the reference:
  - the same mode Selector on OUT 3-4 .. 7-8 too (the app dips every router output on a mode change, MIX-033);
  - one Selector per track over its assignment (the app's assignment dip), always with the app's re-select fix;
  - the assign rules: M + 6CH keeps every track in the master and adds a track assigned to OUT 3-4..7-8 / mono 3..8 to
    that output (OUT 1-2, mono 1/2 and "--" are master only); 8CH sends each track to one output ("--" = OUT 1-2) and
    OUT 1-2 carries only its tracks; mono = (L + R) x 0.5 on one channel; an absent pair folds into Main (M + 6CH:
    dropped, the master has the track; 8CH: OUT 1-2, a mono channel keeping its side);
  - profile "app": re-selecting the mode during its fade-out fades back in from the gain reached (the fix of F209);
    "reference" reproduces the quirk (OutputRouter::setReferenceQuirk).
Start-up: every selector starts settled at the case's initial selection (the reference fades in at start).
Sums follow the app's order: tracks 0..15 added into a cleared pair buffer, then x the mode gain.
"""
from ..f32 import F

NAME = "routing_router"
SUBJECT = "routing.router"
RESEARCH_MODELS = ["headphone_mix", "track_mix"]
OWNER_INPUTS = []
PAIRS = 4
SIDES = ("L", "R")
PAIR_NAMES = ("out12", "out34", "out56", "out78")


def _params(case):
    return case["params"]


def input_channels(case):
    return ["master.L", "master.R"] + ["t%d.%s" % (k, s) for k in range(len(_params(case)["assign"])) for s in SIDES]


def output_channels(case):
    return ["%s.%s" % (p, s) for p in PAIR_NAMES for s in SIDES]


def validate(case):
    p = _params(case)
    unknown = set(p) - {"mode", "assign", "present", "profile", "events"}
    if unknown:
        raise ValueError("unknown params %s" % sorted(unknown))
    if not 1 <= len(p["assign"]) <= 16 or not all(0 <= a <= 12 for a in p["assign"]):
        raise ValueError("assign: 1..16 OutputAssign indices 0..12")
    if p.get("mode", 0) not in (0, 1) or p.get("profile", "reference") not in ("reference", "app"):
        raise ValueError("mode 0|1, profile reference|app")
    for ev in p.get("events", []):
        keys = set(ev) - {"block"}
        if keys not in ({"mode"}, {"track", "assign"}, {"present"}):
            raise ValueError("unknown event %r" % ev)
        if "track" in ev and not (0 <= ev["track"] < len(p["assign"]) and 0 <= ev["assign"] <= 12):
            raise ValueError("event track/assign out of range: %r" % ev)


def resolve(index, mode, present):
    """OutputRouter::resolve for an OutputAssign index -> ("pair", p) | ("mono", ch) | None (master only)."""
    pair = index - 1 if 1 <= index <= 4 else -1
    mono = index - 5 if 5 <= index <= 12 else -1
    if mode == 0:
        if pair >= 1:
            p = pair
        elif mono >= 2:
            p = mono >> 1
        else:
            return None
        if not (present >> p) & 1:
            return None
        return ("pair", pair) if pair >= 0 else ("mono", mono)
    if mono >= 0:
        return ("mono", mono if (present >> (mono >> 1)) & 1 else (mono & 1))
    return ("pair", pair if pair >= 0 and (present >> pair) & 1 else 0)


def settle(sel):
    sel.cur = sel.target                       # start settled (the Selector's smoother is at 1.0)


def app_fix(sel):
    """The app's re-select fix: the selection playing is selected again during its fade-out -> back in from the gain."""
    if sel.cur == sel.target and sel.sm.target == 0.0:
        sel.sm.set_target(1.0, 1, 0)


def _add(acc, x):
    acc[:] = [F(a + b) for a, b in zip(acc, x)]


def run(case, inputs, research, block):
    hm = research.load("headphone_mix")
    p = _params(case)
    quirk = p.get("profile", "reference") == "reference"
    mode = p.get("mode", 0)
    present = (p.get("present", 0x0F) | 1) & 0x0F
    n_tracks = len(p["assign"])
    frames = len(inputs[0])
    # Per track: a Selector over its assignment (selection j = channels 2 + 2j, 3 + 2j of its view).
    tsel = []
    for a in p["assign"]:
        s = hm.Selector(128, True, (2, 3))
        s.select(2 + 2 * a, 3 + 2 * a)
        settle(s)
        tsel.append(s)
    # Per output pair: the mode Selector (selection (2, 3) = the M + 6CH content, (4, 5) = the 8CH content).
    msel = []
    for _ in range(PAIRS):
        s = hm.Selector(128, True, (2, 3))
        s.select(*((2, 3) if mode == 0 else (4, 5)))
        settle(s)
        msel.append(s)
    out = [[] for _ in range(2 * PAIRS)]
    events = p.get("events", [])
    for b in range(frames // block):
        for ev in [e for e in events if e["block"] == b]:
            if "mode" in ev:
                for s in msel:
                    s.select(*((2, 3) if ev["mode"] == 0 else (4, 5)))
            elif "present" in ev:
                present = (ev["present"] | 1) & 0x0F
            else:
                tsel[ev["track"]].select(2 + 2 * ev["assign"], 3 + 2 * ev["assign"])
        sl = slice(b * block, (b + 1) * block)
        master = (inputs[0][sl], inputs[1][sl])
        # 1. each track's feed through its assignment Selector.
        feeds = []
        for t in range(n_tracks):
            L = inputs[2 + 2 * t][sl]; R = inputs[3 + 2 * t][sl]
            m = [F(F(x + y) * 0.5) for x, y in zip(L, R)]     # mono = (L + R) x 0.5
            s = tsel[t]
            app_fix(s)
            chans = [[0.0] * block, [0.0] * block]
            for j in range(13):
                chans += [list(m), list(m)] if j >= 5 else [list(L), list(R)]
            s.process(chans)
            feeds.append(((s.cur[0] - 2) // 2, chans[0], chans[1]))
        # 2. the content of every output pair in either mode (the app sums tracks 0..15 into a cleared buffer).
        content = {}
        for md in (0, 1):
            pairs = [[[0.0] * block, [0.0] * block] for _ in range(PAIRS)]
            for j, fl, fr in feeds:
                d = resolve(j, md, present)
                if d is None:
                    continue
                if d[0] == "pair":
                    _add(pairs[d[1]][0], fl); _add(pairs[d[1]][1], fr)
                else:
                    _add(pairs[d[1] >> 1][d[1] & 1], fl)
            if md == 0:
                pairs[0] = [list(master[0]), list(master[1])]    # M + 6CH: OUT 1-2 is the master
            content[md] = pairs
        # 3. the mode Selector of every pair (Output12Router on OUT 1-2).
        for q in range(PAIRS):
            s = msel[q]
            if not quirk:
                app_fix(s)
            chans = [[0.0] * block, [0.0] * block] + content[0][q] + content[1][q]
            s.process(chans)
            out[2 * q].extend(chans[0]); out[2 * q + 1].extend(chans[1])
    return out, {}
