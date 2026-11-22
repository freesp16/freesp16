#!/usr/bin/env python3
"""The sequencer check on the Linux desktop (build, save, resume and render a beat; run by
scripts/host-smoke.sh CHECK=m2). Two subcommands:

  tools/m2-check.py script --run1 FILE --run2 FILE --out-dir DIR
      writes the two scripted-input sessions (ScriptedInput format, io/controllers/ScriptedInput.h):
      run 1 (with the generated kit on scenes 1-4) builds a 4-scene beat through the panel keys — SCENE mode
      (SCENE, scene key, pattern key), pads to select tracks, step keys for FULL / HALF / PARAM steps (TRIGGER TYPE
      and one pitch lock per pattern through `cmd`, the hold + turn equivalent), 4 scenes x 2 patterns, scene BPMs
      (`cmd scene-bpm`), one recorded pass (REC, PLAY, two pad hits, STOP) on scene 1 pattern 1, then PLAY with
      scenes 2, 3, 4 queued in turn, STOP, `cmd save-as beat4`, `cmd state`, `cmd render DIR/render1.wav 12`, PLAY
      again; the smoke then kills the app (SIGKILL by its pid). Run 2 (no kit) waits for the resume, logs `cmd state`
      and renders DIR/render2.wav, then quits.

  tools/m2-check.py verify --log1 LOG --log2 LOG --render1 WAV --render2 WAV [--load-max 202000]
      checks and prints one line per check (exit 1 when one fails):
        - run 1: the beat was built: patterns 1-2 of scenes 1-4 have FULL, HALF and PARAM steps and a lock; scene
          BPMs set; one recorded pass (2 RecordedHits, written to scene 1 pattern 1); the live queue 1->4 switched
          (M2 SWITCH scene 2, 3, 4); SAVED beat4 ok
        - run 2: "PROJECT resumed beat4"; its M2 STATE / M2 PATTERN lines equal run 1's after the save (selection
          scene/pattern included)
        - the offline renders: same frame count and the same hash (FNV-1a 64 of the float output)
        - switch frames: the RENDER switch frames equal a block-by-block model of the transport and the switcher
          (PATTERN unit: the end of each pass; the new scene's BPM from the next block start)
        - onsets on MAIN of render1.wav (mean of L/R, 2 ms windows, -50 dBFS gate): one per FULL step time of the
          playing pattern (HALF steps fall on silence and start nothing, PARAM steps start nothing), within 5 ms,
          no others
        - live engine: the last LOAD p95 of each run <= --load-max cycles/block, xruns 0
Standard library only."""
import argparse
import math
import re
import struct
import sys
import wave

TAG = re.compile(r"^\[sp16-mod\] \S+ (.*)$")


# --------------------------------------------------------------------------------------------------------- script
def script_cmd(a) -> int:
    ev = []
    t = 3000

    def at(ms, line):
        ev.append((ms, line))

    def key(name, gap=90):
        nonlocal t
        at(t, f"press {name}")
        at(t + 40, f"release {name}")
        t += gap

    def cmd(text, gap=70):
        nonlocal t
        at(t, f"cmd {text}")
        t += gap

    def wait(ms):
        nonlocal t
        t += ms

    def scene_pattern(scene, pattern):
        key("scene")
        key(f"step{scene}")
        key(f"step{pattern}")
        wait(300)  # the switch (immediate while stopped) and its SwitchDone reach the model
        key("scene")
        wait(150)

    for s, bpm in ((1, 120), (2, 100), (3, 90), (4, 110)):
        cmd(f"scene-bpm {s} {bpm}")
    for s in range(1, 5):
        for p in (1, 2):
            scene_pattern(s, p)
            track = s if p == 1 else s + 4
            key(f"pad{track}", 200)  # selects the track (and plays it)
            full, half, param, lock = ([1, 5, 9, 13], 3, 7, (5, 7)) if p == 1 else ([1, 7, 11], 15, 3, (7, -5))
            cmd("trigger-type full")
            for st in full:
                key(f"step{st}")
            cmd("trigger-type half")
            key(f"step{half}")
            cmd("trigger-type param")
            key(f"step{param}")
            cmd("trigger-type full")
            cmd(f"lock {lock[0]} playback.pitch {lock[1]}")
            wait(200)
    # One recorded pass on scene 1 pattern 1, track 9: pad hits on steps 3 and 11 (120 BPM: 125 ms a step).
    scene_pattern(1, 1)
    key("pad9", 300)
    key("rec", 400)
    play = t
    at(play, "press play")
    at(play + 40, "release play")
    for step in (3, 11):
        at(play + (step - 1) * 125, "press pad9 100")
        at(play + (step - 1) * 125 + 40, "release pad9")
    t = play + 2150
    key("stop", 300)
    key("rec", 600)
    # Live: PLAY, then scenes 2, 3, 4 queued in turn (PATTERN unit: each switch at the end of a pass).
    play = t
    at(play, "press play")
    at(play + 40, "release play")
    t = play + 400
    for s, after in ((2, 0), (3, 2400), (4, 2600)):
        wait(after)
        key("scene")
        key(f"step{s}")
        key("step1")
        key("scene")
    t = play + 9800
    key("stop", 400)
    cmd(f"save-as beat4", 2500)
    cmd("state", 300)
    cmd(f"render {a.out_dir}/render1.wav 12", 500)
    at(t, "press play")  # killed while playing
    at(t + 40, "release play")
    at(t + 60000, "quit")  # only if the kill never comes
    with open(a.run1, "w") as f:
        f.write("# generated by tools/m2-check.py script (run 1: build, record, queue, save, render; killed)\n")
        for ms, line in sorted(ev, key=lambda e: e[0]):
            f.write(f"{ms} {line}\n")
    with open(a.run2, "w") as f:
        f.write("# generated by tools/m2-check.py script (run 2: resume, state, render)\n")
        f.write("5000 cmd state\n")
        f.write(f"5300 cmd render {a.out_dir}/render2.wav 12\n")
        f.write("8000 quit\n")
    return 0


# --------------------------------------------------------------------------------------------------------- verify
def lines(path):
    out = []
    with open(path, errors="replace") as f:
        for raw in f:
            m = TAG.match(raw.rstrip("\n"))
            if m:
                out.append(m.group(1))
    return out


def kv(line):
    return dict(re.findall(r"(\w+)=(\S+)", line))


def state_after(log, marker_prefix):
    """The M2 STATE line and the M2 PATTERN lines that follow it (the first STATE after the marker line)."""
    seen = marker_prefix is None
    state, patterns = None, []
    for ln in log:
        if not seen:
            seen = ln.startswith(marker_prefix)
            continue
        if state is None:
            if ln.startswith("M2 STATE "):
                state = ln
            continue
        if ln.startswith("M2 PATTERN "):
            patterns.append(ln)
        elif patterns or not ln.startswith("M2 "):
            if not ln.startswith("M2 PATTERN"):
                break
    return state, patterns


class Model:
    """Block-by-block transport + PATTERN-unit switcher, as the engine runs them (64-frame blocks)."""

    def __init__(self, rate, bpm):
        self.rate, self.bpm = rate, bpm
        self.frames = 0
        self.ppq = 0.0

    def ppf(self):
        return self.bpm / (60.0 * self.rate)


def expected_render(rate, total, scenes):
    """scenes: [(bpm, length_steps, [full step numbers 1-based])] for scenes 1..4 (pattern 1). Returns the switch frames
    (scenes 2..4) and the onset frames."""
    bpm = scenes[0][0]
    ppq0 = 0.0  # PPQ at frame `base`
    base = 0
    scene = 0
    start = 0.0  # pass start (PPQ)
    switches, onsets = [], []
    pending_tempo = None
    pending_queue = None  # scene 2 is queued after the first block (Document::Impl::render)
    queued = None
    block = 0
    while block * 64 < total:
        if pending_tempo is not None:  # SetTempo from the switch: from this block start (rebased)
            ppq0 = ppq0 + (block * 64 - base) * bpm / (60.0 * rate)
            base = block * 64
            bpm = pending_tempo
            pending_tempo = None
        if block == 1:
            pending_queue = 1
        if pending_queue is not None:
            queued, pending_queue = pending_queue, None
        ppf = bpm / (60.0 * rate)
        p0 = ppq0 + (block * 64 - base) * ppf
        p1 = p0 + 64 * ppf
        length_q = scenes[scene][1] * 0.25
        if p0 >= start + length_q - 1e-9:  # a wrap
            start += math.floor((p0 - start) / length_q + 1e-9) * length_q
        b_frame = None
        if queued is not None:
            k = math.ceil((p0 - start) / length_q - 1e-9)
            b = start + max(k, 0) * length_q
            if (b - p0) / ppf + 1e-6 < 64:
                b_frame = min(63, max(0, int((b - p0) / ppf + 1e-6)))
        # onsets of the pattern(s) playing in this block
        segs = []
        if b_frame is None:
            segs.append((scene, start, p0, p1))
        else:
            segs.append((scene, start, p0, b))
            segs.append((queued, b, b, p1))
        for sc, st, a, z in segs:
            length_q2 = scenes[sc][1] * 0.25
            for n in range(int(math.floor((a - st) / 0.25)) - 1, int(math.ceil((z - st) / 0.25)) + 1):
                if n < 0:
                    continue
                step = n % scenes[sc][1] + 1
                if step in scenes[sc][2]:
                    t = st + n * 0.25
                    if a - 1e-9 <= t < z - 1e-9:
                        onsets.append(block * 64 + min(63, max(0, int((t - p0) / ppf + 1e-6))))
        if b_frame is not None:
            switches.append(block * 64 + b_frame)
            scene, start = queued, b
            if scenes[scene][0] != bpm:
                pending_tempo = scenes[scene][0]
            queued = None
            if scene < len(scenes) - 1:
                pending_queue = scene + 1  # posted after this block's SwitchDone: from the next block
        block += 1
    return switches, sorted(set(onsets))


def read_main(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit(f"{path}: {width * 8}-bit not supported")
    data = struct.unpack("<%dh" % (n * ch), raw[: n * ch * 2])
    return rate, [(data[i * ch] + data[i * ch + 1]) / 65536.0 for i in range(n)]


def detect_onsets(main, rate, gate_dbfs=-50.0):
    win = max(1, int(0.002 * rate))
    gate = 10 ** (gate_dbfs / 20)
    env = [max((abs(x) for x in main[i:i + win]), default=0.0) for i in range(0, len(main), win)]
    join = int(0.020 * rate / win) + 1
    out, i = [], 0
    while i < len(env):
        if env[i] < gate:
            i += 1
            continue
        # refine inside the window: the first sample above the gate
        s0 = i * win
        while s0 < (i + 1) * win and abs(main[s0]) < gate:
            s0 += 1
        out.append(s0)
        j, quiet = i, 0
        while j < len(env) and quiet < join:
            quiet = quiet + 1 if env[j] < gate else 0
            j += 1
        i = j
    return out


def verify_cmd(a) -> int:
    ok = True

    def check(cond, text):
        nonlocal ok
        print(("  OK   " if cond else "  FAIL ") + text)
        ok = ok and bool(cond)

    log1, log2 = lines(a.log1), lines(a.log2)
    # ---- run 1: the beat
    state1, pats1 = state_after(log1, "SAVED beat4")
    check(any(l.startswith("SAVED beat4 ok") for l in log1), "run 1: SAVED beat4 ok")
    check(state1 is not None, f"run 1: state after the save: {state1}")
    by = {}
    for p in pats1:
        d = kv(p)
        by[(int(d["scene"]), int(d["pattern"]))] = d
    built = all((s, q) in by and int(by[(s, q)]["full"]) > 0 and int(by[(s, q)]["half"]) > 0
                and int(by[(s, q)]["param"]) > 0 and int(by[(s, q)]["locks"]) > 0
                for s in range(1, 5) for q in (1, 2))
    check(built, "run 1: scenes 1-4 x patterns 1-2 hold FULL, HALF and PARAM steps and a lock each")
    bpms = [float(by.get((s, 1), {}).get("bpm", 0)) for s in range(1, 5)]
    check(bpms == [120.0, 100.0, 90.0, 110.0], f"run 1: scene BPMs {bpms}")
    rec = kv(state1 or "").get("recorded")
    s1p1 = by.get((1, 1), {}).get("cells", "")
    check(rec == "2" and "F3.9" in s1p1 and "F11.9" in s1p1,
          f"run 1: one recorded pass: {rec} RecordedHits, track 9 steps 3 and 11 in scene 1 pattern 1")
    live = [int(kv(l)["scene"]) for l in log1 if l.startswith("M2 SWITCH ")]
    # the queue 1 -> 4 while playing comes after the recorded pass: the last three switches
    check(live[-3:] == [2, 3, 4], f"run 1: live queue 1->4 switched (last switches {live[-3:]})")
    check(not any(l == "STOP" for l in log1), "run 1: ended by the kill (no clean STOP)")
    # ---- run 2: the resume
    check(any(l.startswith("PROJECT resumed beat4") for l in log2), "run 2: PROJECT resumed beat4")
    state2, pats2 = state_after(log2, None)
    strip = lambda s: re.sub(r" recorded=\d+", "", s or "")
    check(state2 is not None and strip(state1) == strip(state2),
          f"run 2: selection and tempo as saved ({strip(state2)})")
    check(pats1 == pats2 and len(pats2) >= 8, f"run 2: the {len(pats2)} patterns with content equal run 1's")
    # ---- offline renders
    r1 = [kv(l) for l in log1 if l.startswith("RENDER file=")]
    r2 = [kv(l) for l in log2 if l.startswith("RENDER file=")]
    check(r1 and r2, "renders: both runs rendered")
    if r1 and r2:
        check(r1[-1]["hash"] == r2[-1]["hash"] and r1[-1]["frames"] == r2[-1]["frames"],
              f"renders: hash {r1[-1]['hash']} / {r2[-1]['hash']}, frames {r1[-1]['frames']} / {r2[-1]['frames']}")
        rate = float(r1[-1]["rate"])
        total = int(r1[-1]["frames"])
        scenes = []
        for s in range(1, 5):
            d = by.get((s, 1), {})
            fulls = sorted({int(c[1:].split(".")[0]) for c in d.get("cells", "").split(",") if c.startswith("F")})
            scenes.append((float(d.get("bpm", 120)) or 120.0, int(d.get("length", 16)), fulls))
        exp_sw, exp_on = expected_render(rate, total, scenes)
        got_sw = [int(kv(l)["frame"]) for l in log1 if l.startswith("RENDER switch ")][-3:]
        check(got_sw == exp_sw[:3], f"switch frames: render {got_sw}, expected (pass ends) {exp_sw[:3]}")
        r, main = read_main(a.render1)
        got = detect_onsets(main, r)
        tol = int(0.005 * r)
        missing = [e for e in exp_on if not any(abs(g - e) <= tol for g in got)]
        extra = [g for g in got if not any(abs(g - e) <= tol for e in exp_on)]
        err = [min((g - e for g in got), key=abs) for e in exp_on if any(abs(g - e) <= tol for g in got)]
        worst = max((abs(x) for x in err), default=0)
        check(not missing and not extra,
              f"onsets on MAIN: {len(got)} found, {len(exp_on)} expected (FULL steps), missing {missing[:4]}, "
              f"extra {extra[:4]}, worst {worst / r * 1000:.1f} ms")
    # ---- live engine
    for n, log in (("run 1", log1), ("run 2", log2)):
        loads = [kv(l) for l in log if l.startswith("LOAD ")]
        if not loads:
            check(False, f"{n}: no LOAD line")
            continue
        p95 = float(loads[-1]["p95"])
        xr = [int(kv(l)["xruns"]) for l in log if l.startswith("AUDIO alsa_periods=")]
        check(p95 <= a.load_max and int(loads[-1]["xruns"]) == 0 and all(x == 0 for x in xr),
              f"{n}: LOAD p95={p95:.0f} cycles/block (limit {a.load_max}), xruns {loads[-1]['xruns']}")
    print("m2 check:", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("script")
    s.add_argument("--run1", required=True)
    s.add_argument("--run2", required=True)
    s.add_argument("--out-dir", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--log1", required=True)
    v.add_argument("--log2", required=True)
    v.add_argument("--render1", required=True)
    v.add_argument("--render2", required=True)
    v.add_argument("--load-max", type=float, default=202000)
    a = ap.parse_args()
    return script_cmd(a) if a.cmd == "script" else verify_cmd(a)


if __name__ == "__main__":
    sys.exit(main())
