#!/usr/bin/env python3
"""Long soak of the real app (NFR-014 "24 h soak: no xruns, no memory growth > 10 MB, no stuck notes",
NFR-010 "no data loss on crash or power loss", the load budget). Scripted, no human, on the Linux desktop and on
the unit. Standard library only. Subcommands:

  tools/soak/soak-run.py script --out DIR [--hours H] [--seed S]
      writes DIR/script.txt (ScriptedInput format, io/controllers/ScriptedInput.h) and DIR/manifest.json: the live
      set (tools/liveset-check.py's generator: panel controls and UI-120 keys only) stretched to H hours, plus the soak's
      own routines on a clock (our periods, all from the script's time 0):
        - save (PROJECT > SAVE on the panel)              every --save-min   (default 10 min)
        - load (PROJECT > OPEN > the saved project > OPEN) 30 s after each save: the undo history and the whole model
                                                          are replaced, the transport restarts
        - render (`cmd render-job pattern bars=1`)        every --render-min (default 10 min, offset by half a period)
        - song mode (`cmd arr-*`: a 3-row arrangement, arrangement.loop on, PLAY follows it for ~90 s, then back to
          pattern mode)                                   every --song-min   (default 15 min)
        - scene / pattern switches, chains, knob walks, FX-module screens, step edits, undo / redo, recording passes,
          mutes / solos, tempo changes … (the live set's random routines, --gap-ms between them)
      The same seed gives the same script. On the unit: copy script.txt next to the app and start it with
      input_script=<path> (tools/soak/soak-unit.sh does the monitoring).

  tools/soak/soak-run.py run [--hours H] [--seed S] [--preset host-fast] [--render soft] [--out DIR]
      the desktop soak: the generated kit (tools/gen-test-kit.py) on scenes 1-2, the app under xvfb-run with the
      linux platform and audio_device=file:<FIFO> (the FIFO is read and analysed here, so a 24 h run writes no 600 MB
      per hour of WAV), the script above. While it runs: one sample of /proc/<pid> every --interval s (VmRSS, VmHWM,
      threads, open fds, CPU) into monitor.csv, the app's LOAD / AUDIO / SAVED / RENDER lines, the autosave folder's
      writes, MAIN's peak per 100 ms; one SOAK summary line every --summary-min minutes (default 10) on stdout and in
      soak.txt. Then the kill test (--kill-cycles, default 3): the app is started again on the same data folder, a save
      is started on the panel and the app is SIGKILLed (by its exact PID) at a random moment around it; a third start
      must resume the project with no leftovers of the interrupted save. Exit 0 = every check passed.
      --heap-stats: the app also runs with tools/soak/heapstat.c preloaded (built here with cc; glibc's main-arena
      in-use / free bytes every 5 s into heap.csv), so a rising RSS is told apart: a leak (in-use grows) or allocator
      fragmentation (arena and free grow, in-use flat). --heap-trim adds malloc_trim(0) once a minute (the
      experiment for the fragmentation case). Desktop only.

  tools/soak/soak-run.py analyze --log LOG [--monitor monitor.csv] [--manifest manifest.json] [--unit]
      the same checks on a finished run's files (the unit: LOG = the app's stdout, monitor.csv from soak-unit.sh;
      --unit gates LoadMeter overruns too, which the desktop only reports).

Checks (each one PASS / FAIL line, then one table; thresholds in the constants below):
  exit code 0, no FATAL, no REALTIME VIOLATION (APP_RT_CHECKS aborts on an audio-thread allocation)
  xruns = 0 in every AUDIO / LOAD line; LOAD p95 <= 725 000 cycles/block (50 % of the 64-frame block at 44.1 kHz on
    the A9's 1 GHz; the desktop's LoadMeter counts 1 cycle = 1 ns, so there it is a relative gate) in every LOAD line;
    LoadMeter overruns: gated with --unit, reported on the desktop (Xvfb + the soft renderer share the cores)
  RSS growth <= 10 MB (NFR-014): the median VmRSS of the last 10 % of the run minus the median VmRSS of the window
    after the warm-up (the first max(10 min, 10 %) — the kit, caches and the first undo history are in place); the
    slope over the post-warm-up part (MB/h) is reported. Each load replaces the model and its undo history, so a flat
    line is expected; a leak per event or per block shows as a slope
  fds and threads flat: the median of the last 10 % <= the median after the warm-up + 2
  every save ok (SAVED … ok, no FAILED); every load reached the OPEN PROJECT screen; every render job done
  unsaved work covered: the longest stretch with no save, open or autosave write while edits continue <= 360 s
    (PRJ-030: the autosave writes 5 min after the first unsaved change while the transport plays, 60 s while stopped;
    + 60 s: the copy is due 5 min after the first edit that follows a save or an open). NFR-010's "autosave
    <= 60 s old" holds for the stopped transport; while playing the 5-minute rule of PRJ-030 applies (our reading)
  no stuck notes: MAIN peak <= -70 dBFS over the last 3 s (after the final STOP, STOP); the set sounded (> -30 dBFS)
  kill test (run only): every relaunch after SIGKILL logs "PROJECT resumed <name>", the project folder holds no
    staging / .new / .bak / .commit leftovers after that start, and its state checkpoint loads
"""
import argparse
import array
import collections
import datetime
import importlib.util
import json
import math
import os
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

LOAD_P95_MAX = 725000          # cycles/block: 50 % of 64 / 44100 s at 1 GHz
LOAD_MAX_MAX = 1451247         # cycles/block: 100 % of the block (reported; an overrun is a block above it)
RSS_GROWTH_MAX_MB = 10.0       # NFR-014
FLAT_SLACK = 2                 # fds / threads
UNSAVED_GAP_MAX_S = 360.0      # PRJ-030 playing interval 300 s + up to 60 s to the first edit after a save / open
TAIL_S, TAIL_MAX_DBFS, ACTIVE_MIN_DBFS = 3.0, -70.0, -30.0
SAVE_LOAD_GAP_MS = 30000       # a load starts 30 s after its save (the store refuses overlapping jobs)


def load_liveset():
    spec = importlib.util.spec_from_file_location("liveset_check", os.path.join(ROOT, "tools", "liveset-check.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def identity():
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "identity.py"), "--shell"], check=True,
                         capture_output=True, text=True).stdout
    ids = {}
    for line in out.splitlines():
        m = re.match(r"export (\w+)=(.*)$", line)
        if m:
            ids[m.group(1)] = m.group(2).strip("'")
    return ids


# --- the script --------------------------------------------------------------------------------------------------------
def make_gen_class(ls):
    class SoakGen(ls.Gen):
        def __init__(self, seed, hours, save_min, render_min, song_min, gap_ms):
            super().__init__(seed, hours * 60.0)
            self.hours = hours
            self.save_ms, self.render_ms, self.song_ms = (int(x * 60000) for x in (save_min, render_min, song_min))
            self.gap_lo, self.gap_hi = gap_ms
            self.loads, self.renders, self.songs = [], [], []
            self.saved_state = None
            self.project_saved = False

        # PROJECT > SAVE; remembers what the project holds at that moment (the load brings it back)
        def r_save(self):
            super().r_save()
            self.saved_state = (self.bpm, self.pattern, self.scene, self.length_bars)
            self.project_saved = True

        def r_load(self):
            self.t = max(self.t, self.last_save_ms + SAVE_LOAD_GAP_MS)
            self.last_save_ms = self.t           # the next save keeps its distance from this load too
            self.begin("load")
            self.loads.append(self.t)
            self.key("ext.project", gap=400)     # PROJECT
            self.screen("project")
            self.turn("sel", -20, gap=100)       # CREATE NEW …
            self.turn("sel", 1, gap=150)         # … OPEN
            self.key("sel.push", gap=700)        # OPEN PROJECT: newest first (sorted by MODIFIED), the soak's project
            self.screen("project.open")
            self.key("sel.push", gap=500)        # "Open …?" (+ the unsaved-changes warning)
            self.key("sel.push", gap=3000)       # OPEN (the affirmative button is focused): LOADING, then HOME
            self.feat("project.open")
            self.home(gap=500)
            self.home()
            self.bpm, self.pattern, self.scene, self.length_bars = self.saved_state
            self.key("stop", gap=300)            # whatever the open left: stop, silence, play from the top
            self.key("stop", gap=300)
            self.key("play", gap=400)
            self.playing = True

        def r_render(self):
            self.begin("render")
            self.renders.append(self.t)
            self.cmd("render-job pattern bars=1 totrack=0 normalize=0", gap=300)
            self.feat("render.job")

        def r_song(self):
            # song mode looping (NFR-014): a 3-row arrangement over scenes 1-2, ARRANGEMENT END = LOOP, PLAY follows it
            self.begin("song")
            self.songs.append(self.t)
            for c in ("arr-clear", "arr-append 1 1 1 0", "arr-append 1 2 1 0", "arr-append 2 1 2 0",
                      "param arrangement.loop 0 1", "arr-play 1"):
                self.cmd(c, gap=150)
            self.key("stop", gap=300)
            self.key("stop", gap=300)
            self.key("play", gap=400)            # the arrangement from row 1, looping
            self.feat("song.loop")
            end = self.t + 90000
            safe = [self.r_pads, self.r_home_knobs, self.r_mute_solo, self.r_lfo, self.r_amp, self.r_strip,
                    self.r_select]
            while self.t < end:
                self.rng.choice(safe)()
                self.wait(self.rng.randint(self.gap_lo, self.gap_hi))
            self.cmd("arr-state", gap=150)
            self.cmd("arr-play 0", gap=150)
            self.key("stop", gap=300)
            self.key("stop", gap=300)
            # back to pattern mode on scene 1, pattern 1 (where the song left the current pattern is not tracked here)
            self.key("scene", gap=150)
            self.key("step1", gap=150)
            self.key("step1", gap=150)
            self.key("scene", gap=150)
            self.scene, self.pattern = 1, 1
            self.key("play", gap=400)
            self.playing = True

        def build(self):
            self.t = 4000
            self.r_start()
            first = [self.r_pads, self.r_steps, self.r_home_knobs, self.r_pad_modes, self.r_mute_solo, self.r_lfo,
                     self.r_amp, self.r_sequence, self.r_set_slice, self.r_set_scale, self.r_strip_setting,
                     self.r_strip, self.r_length, self.r_steps, self.r_length, self.r_scene_pattern, self.r_chain,
                     lambda: self.r_copy_paste(True), self.r_tempo, self.r_record, self.r_transport, self.r_erase,
                     self.r_select, self.r_scene_manager, self.r_menus, self.r_bpm, self.r_save, self.r_checkpoint,
                     self.r_load, self.r_render, self.r_checkpoint]
            for r in first:
                r()
                self.wait(400)
            self.cycle1_ms = self.t
            pool = [(self.r_pads, 10), (self.r_steps, 6), (self.r_home_knobs, 3), (self.r_pad_modes, 3),
                    (self.r_mute_solo, 3), (self.r_lfo, 3), (self.r_amp, 2), (self.r_sequence, 3),
                    (self.r_set_slice, 2), (self.r_set_scale, 1), (self.r_strip_setting, 1), (self.r_strip, 4),
                    (self.r_length, 1), (self.r_scene_pattern, 3), (self.r_chain, 1), (self.r_copy_paste, 3),
                    (self.r_tempo, 2), (self.r_record, 3), (self.r_transport, 2), (self.r_erase, 1),
                    (self.r_select, 2), (self.r_scene_manager, 1), (self.r_menus, 1), (self.r_bpm, 1)]
            funcs, weights = [f for f, _ in pool], [w for _, w in pool]
            end = int(self.minutes * 60000)
            next_save = self.save_ms
            next_render = self.render_ms // 2
            next_song = self.song_ms
            last_state = self.t
            while self.t < end - 90000:
                if self.t >= next_save:
                    self.r_save()
                    self.r_load()
                    next_save += self.save_ms
                elif self.t >= next_render:
                    self.r_render()
                    next_render += self.render_ms
                elif self.t >= next_song and self.t + 100000 < end - 90000:
                    self.r_song()
                    next_song += self.song_ms
                else:
                    self.rng.choices(funcs, weights)[0]()
                self.wait(self.rng.randint(self.gap_lo, self.gap_hi))
                if self.t - last_state > 3 * 60000:
                    self.r_checkpoint()
                    last_state = self.t
            # the end (as the live set's): pattern length back to 1 bar, a last save, STOP, STOP, checkpoints, quit
            self.t = max(self.t, end - 50000)
            if self.length_bars != 1:
                self.r_length()
            self.r_save()
            self.t = max(self.t, end - 12000)
            self.begin("end")
            self.key("stop", gap=500)
            self.key("stop", gap=300)
            self.playing = False
            self.feat("transport.stop", "transport.hardStop")
            self.stop_ms = self.t
            self.t = max(self.t, end - 6000)
            self.cmd("state", gap=200)
            self.t = max(self.t, end)
            self.quit_ms = self.t + 4000
            self.at(self.quit_ms, "quit")

        def write(self, out, manifest):
            m = super().write(out, manifest)
            m.update({"hours": self.hours, "loads": self.loads, "renders": self.renders, "songs": self.songs,
                      "save_ms": self.save_ms, "render_ms": self.render_ms, "song_ms": self.song_ms,
                      "save_times": [r["at"] for r in self.routines if r["name"] == "save"],
                      "generator": "tools/soak/soak-run.py"})
            with open(manifest, "w") as f:
                json.dump(m, f, indent=1)
            with open(out) as f:
                text = f.read()
            with open(out, "w") as f:
                f.write(f"# tools/soak/soak-run.py soak: hours={self.hours:g} seed={self.seed} saves={len(self.saves)} "
                        f"loads={len(self.loads)} renders={len(self.renders)} songs={len(self.songs)}\n" + text)
            return m

    return SoakGen


def generate(out_dir, hours, seed, save_min=10.0, render_min=10.0, song_min=15.0, gap_ms=(200, 1200)):
    os.makedirs(out_dir, exist_ok=True)
    ls = load_liveset()
    gen = make_gen_class(ls)(seed, hours, save_min, render_min, song_min, gap_ms)
    gen.build()
    script, manifest = os.path.join(out_dir, "script.txt"), os.path.join(out_dir, "manifest.json")
    m = gen.write(script, manifest)
    return script, manifest, m


def script_cmd(a):
    script, manifest, m = generate(a.out, a.hours, a.seed, a.save_min, a.render_min, a.song_min, parse_gap(a.gap_ms))
    print(f"soak script: {script} hours={a.hours:g} seed={a.seed} events={m['events']} saves={len(m['saves'])} "
          f"loads={len(m['loads'])} renders={len(m['renders'])} songs={len(m['songs'])} quit={m['quit_ms'] / 1000:.0f} s")
    return 0


def parse_gap(text):
    lo, hi = (int(x) for x in text.split(","))
    return lo, hi


# --- log parsing (shared by run and analyze) -----------------------------------------------------------------------
TAG = "[sp16-mod] "
RE_LOAD = re.compile(r"LOAD cycles/block avg=(\S+) p95=(\S+) max=(\S+) load=(\S+)% overruns=(\d+) periods=(\d+) "
                     r"xruns=(\d+) VmRSS=(\d+|n/a)")
RE_AUDIO = re.compile(r"AUDIO alsa_periods=\d+ xruns=(\d+)")


class LogState:
    """Everything the checks need from the app's log, fed line by line (live while the soak runs)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.lines = 0
        self.loads = []          # (wall_s, p95, max, overruns, xruns, rss_kb)
        self.audio_xruns = 0
        self.fatal, self.rt = [], []
        self.saves_ok, self.saves_failed = [], []
        self.save_times = []     # wall_s of SAVED ok
        self.open_times = []     # wall_s of each project open (the project on disk replaces the edited one)
        self.est_s = 0.0         # time since start estimated from the LOAD cadence (one line per 10 s): offline logs
        self.save_est, self.open_est = [], []
        self.opens = 0           # NAV project.open
        self.loaded = 0          # PROJECT <name> samples=… (the start-up's and every open's)
        self.render_done, self.render_failed, self.render_start = 0, 0, 0
        self.states = 0
        self.arr_ok, self.arr_bad = 0, 0
        self.project_lines = []
        self.run_seen = None
        self.script = None       # "ok …" / the parse errors (SCRIPT line)
        self.stop_seen = False
        self.t0 = time.time()

    def feed(self, raw, wall=None):
        wall = time.time() if wall is None else wall
        with self.lock:
            self.lines += 1
            if "REALTIME VIOLATION" in raw:
                self.rt.append(raw.strip())
            i = raw.find(TAG)
            if i < 0:
                return
            rest = raw[i + len(TAG):].strip()
            rest = rest.split(" ", 1)[1] if " " in rest else ""   # the env prefix word
            if rest.startswith("LOAD "):
                self.est_s += 10.0
                m = RE_LOAD.search(rest)
                if m:
                    rss = int(m.group(8)) if m.group(8) != "n/a" else None
                    self.loads.append((wall, float(m.group(2)), float(m.group(3)), int(m.group(5)), int(m.group(7)),
                                       rss))
            elif rest.startswith("AUDIO "):
                m = RE_AUDIO.search(rest)
                if m:
                    self.audio_xruns = max(self.audio_xruns, int(m.group(1)))
            elif rest.startswith("FATAL"):
                self.fatal.append(rest)
            elif rest.startswith("SAVED "):
                (self.saves_ok if " ok" in rest else self.saves_failed).append(rest)
                if " ok" in rest:
                    self.save_times.append(wall)
                    self.save_est.append(self.est_s)
            elif rest.startswith("NAV project.open "):
                self.opens += 1
            elif rest.startswith("RENDER JOB start"):
                self.render_start += 1
            elif rest.startswith("RENDER JOB done cancelled=0"):
                self.render_done += 1
            elif rest.startswith("RENDER JOB failed") or rest.startswith("RENDER JOB refused"):
                self.render_failed += 1
            elif rest.startswith("M2 STATE "):
                self.states += 1
            elif rest.startswith("ARR arr-"):
                if rest.endswith(" ok"):
                    self.arr_ok += 1
                elif not rest.startswith("ARR arr-clear "):   # clearing an empty arrangement is refused (nothing to do)
                    self.arr_bad += 1
            elif rest.startswith("PROJECT "):
                self.project_lines.append(rest)
                if " samples=" in rest:
                    self.loaded += 1
                    if self.loaded > 1:   # not the start-up's
                        self.open_times.append(wall)
                        self.open_est.append(self.est_s)
            elif rest.startswith("SCRIPT "):
                self.script = rest[7:]
            elif rest.startswith("RUN "):
                self.run_seen = wall
            elif rest.startswith("STOP"):
                self.stop_seen = True


class AudioState:
    """MAIN L/R of the file device's 16-bit stereo stream, read from a FIFO: peak per 100 ms window."""

    def __init__(self):
        self.lock = threading.Lock()
        self.frames = 0
        self.rate = 44100
        self.channels = 2
        self.peak_all = 0
        self.peak_window = 0     # since the last summary
        self.recent = collections.deque(maxlen=600)   # the last 60 s of 100-ms peaks
        self.silent_run, self.longest_silent = 0, 0   # consecutive all-zero 100-ms windows
        self.error = None
        self.done = False

    def read(self, path):
        try:
            with open(path, "rb") as f:
                head = f.read(44)
                if len(head) < 44 or head[:4] != b"RIFF":
                    self.error = "no WAV header on the FIFO"
                    return
                self.channels = int.from_bytes(head[22:24], "little")
                self.rate = int.from_bytes(head[24:28], "little")
                win = max(1, self.rate // 10) * self.channels * 2
                buf = b""
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    buf += chunk
                    while len(buf) >= win:
                        self._window(buf[:win])
                        buf = buf[win:]
        except OSError as e:
            self.error = str(e)
        finally:
            self.done = True

    def _window(self, data):
        a = array.array("h")
        a.frombytes(data)
        if sys.byteorder != "little":
            a.byteswap()
        ch = self.channels
        pk = 0
        for c in range(min(2, ch)):
            s = a[c::ch]
            pk = max(pk, max(s), -min(s))
        with self.lock:
            self.frames += len(a) // ch
            self.peak_all = max(self.peak_all, pk)
            self.peak_window = max(self.peak_window, pk)
            self.recent.append(pk)
            if pk == 0:
                self.silent_run += 1
                self.longest_silent = max(self.longest_silent, self.silent_run)
            else:
                self.silent_run = 0


def dbfs(v):
    return 20 * math.log10(v / 32768.0) if v > 0 else float("-inf")


# --- /proc sampling ---------------------------------------------------------------------------------------------------
def proc_sample(pid):
    try:
        st = {}
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                k, _, v = line.partition(":")
                st[k] = v.strip()
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])   # utime + stime (fields 14, 15 of stat)
        fds = len(os.listdir(f"/proc/{pid}/fd"))
        return {"rss_kb": int(st.get("VmRSS", "0 kB").split()[0]), "hwm_kb": int(st.get("VmHWM", "0 kB").split()[0]),
                "threads": int(st.get("Threads", "0")), "fds": fds, "ticks": ticks}
    except (OSError, ValueError, IndexError):
        return None


def newest_mtime(folder):
    best = 0.0
    try:
        for e in os.scandir(folder):
            if e.name.endswith(".json"):
                best = max(best, e.stat().st_mtime)
    except OSError:
        pass
    return best


MON_HEADER = "t_s,rss_kb,hwm_kb,threads,fds,ticks,autosave_mtime"


def read_monitor(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("t_s") or line.startswith("#"):
                continue
            parts = line.split(",")
            try:
                rows.append({"t": float(parts[0]), "rss_kb": int(parts[1]), "hwm_kb": int(parts[2] or 0),
                             "threads": int(parts[3]), "fds": int(parts[4]), "ticks": int(parts[5] or 0),
                             "autosave": float(parts[6]) if len(parts) > 6 and parts[6] else 0.0})
            except (ValueError, IndexError):
                continue
    return rows


# --- the checks -------------------------------------------------------------------------------------------------------
class Checks:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, value, limit):
        self.rows.append((name, ok, value, limit))
        print(f"  {'PASS' if ok is True else ('FAIL' if ok is False else 'INFO')}  {name}: {value} ({limit})")

    def failed(self):
        return [r for r in self.rows if r[1] is False]

    def table(self):
        w = max(len(r[0]) for r in self.rows) if self.rows else 10
        out = [f"{'check'.ljust(w)}  result  value  (limit)", "-" * (w + 40)]
        for name, ok, value, limit in self.rows:
            res = "PASS" if ok is True else ("FAIL" if ok is False else "info")
            out.append(f"{name.ljust(w)}  {res:<6}  {value}  ({limit})")
        return "\n".join(out)


def window_median(rows, key, a, b):
    vals = [r[key] for r in rows if a <= r["t"] <= b]
    return statistics.median(vals) if vals else None


def slope_per_hour(rows, key, from_t):
    pts = [(r["t"], r[key]) for r in rows if r["t"] >= from_t]
    if len(pts) < 3:
        return 0.0
    mx = sum(p[0] for p in pts) / len(pts)
    my = sum(p[1] for p in pts) / len(pts)
    den = sum((p[0] - mx) ** 2 for p in pts)
    return 0.0 if den == 0 else sum((p[0] - mx) * (p[1] - my) for p in pts) / den * 3600.0


def evaluate(log, mon, manifest, exit_code, audio, unit=False, checks=None, gap_gate=True, heap=None):
    c = checks or Checks()
    if exit_code is not None:
        c.add("exit code", exit_code == 0, exit_code, "0")
    c.add("FATAL lines", not log.fatal, len(log.fatal), "0" + (f": {log.fatal[0]}" if log.fatal else ""))
    c.add("REALTIME VIOLATION", not log.rt, len(log.rt), "0 (APP_RT_CHECKS)")
    if manifest:
        steps = re.search(r"ok .*: (\d+) steps", log.script or "")
        c.add("script parsed", bool(steps) and int(steps.group(1)) == manifest.get("events"),
              (log.script or "no SCRIPT line")[:60] + ("…" if log.script and len(log.script) > 60 else ""),
              f"SCRIPT ok, {manifest.get('events')} steps")
    xr = max([x[4] for x in log.loads] + [log.audio_xruns, 0])
    c.add("xruns", xr == 0, xr, "0 in every AUDIO / LOAD line")
    if log.loads:
        p95 = max(x[1] for x in log.loads)
        mx = max(x[2] for x in log.loads)
        over = log.loads[-1][3]
        c.add("LOAD p95 (worst window)", p95 <= LOAD_P95_MAX, f"{p95:.0f} cycles/block",
              f"<= {LOAD_P95_MAX}; median window {statistics.median(x[1] for x in log.loads):.0f}")
        c.add("LOAD max (worst window)", None if not unit else mx < LOAD_MAX_MAX, f"{mx:.0f} cycles/block",
              f"< {LOAD_MAX_MAX} on the unit; reported on the desktop")
        c.add("LoadMeter overruns", (over == 0) if unit else None, over,
              "0 on the unit; reported on the desktop (shared cores)")
        c.add("LOAD lines", len(log.loads) > 0, len(log.loads), "one per 10 s")
    else:
        c.add("LOAD lines", False, 0, "one per 10 s")
    if mon:
        t_end = mon[-1]["t"]
        warm = max(600.0, 0.1 * t_end)
        base_a, base_b = warm, warm + max(300.0, 0.05 * t_end)
        last_a = t_end - max(300.0, 0.1 * t_end)
        if base_b >= last_a:   # a short run: the first and the last third
            base_a, base_b, last_a = t_end / 6, t_end / 3, 2 * t_end / 3
        rss_b = window_median(mon, "rss_kb", base_a, base_b)
        rss_l = window_median(mon, "rss_kb", last_a, t_end)
        if rss_b is not None and rss_l is not None:
            growth = (rss_l - rss_b) / 1024.0
            c.add("RSS growth after warm-up", growth <= RSS_GROWTH_MAX_MB,
                  f"{growth:+.1f} MB ({rss_b / 1024:.1f} -> {rss_l / 1024:.1f} MB; slope "
                  f"{slope_per_hour(mon, 'rss_kb', base_a) / 1024:+.2f} MB/h; peak "
                  f"{max(r['rss_kb'] for r in mon) / 1024:.1f} MB)", f"<= {RSS_GROWTH_MAX_MB:g} MB (NFR-014)")
            if heap:
                t0 = mon[0]["wall"] if "wall" in mon[0] else 0.0
                rows = [{"t": h["wall"] - t0, **h} for h in heap]
                hb = window_median(rows, "in_use", base_a, base_b)
                hl = window_median(rows, "in_use", last_a, t_end)
                fb = window_median(rows, "free", base_a, base_b)
                fl = window_median(rows, "free", last_a, t_end)
                if hb is not None and hl is not None:
                    c.add("heap in use growth (main arena)", None,
                          f"{(hl - hb) / 1048576:+.1f} MB ({hb / 1048576:.1f} -> {hl / 1048576:.1f} MB); free inside "
                          f"the arena {fb / 1048576:.1f} -> {fl / 1048576:.1f} MB",
                          "a leak grows in use; fragmentation grows free")
            for key, label in (("fds", "open fds"), ("threads", "threads")):
                b = window_median(mon, key, base_a, base_b)
                last = window_median(mon, key, last_a, t_end)
                c.add(f"{label} flat", last <= b + FLAT_SLACK,
                      f"{b:g} -> {last:g} (max {max(r[key] for r in mon)})", f"last <= warm-up + {FLAT_SLACK}")
        else:
            c.add("RSS growth after warm-up", False, "too few samples", "monitor.csv")
        hours = t_end / 3600.0
        cpu = (mon[-1]["ticks"] - mon[0]["ticks"]) / os.sysconf("SC_CLK_TCK") / max(1.0, t_end - mon[0]["t"]) * 100
        c.add("CPU (app, all threads)", None, f"{cpu:.0f} % of one core", f"over {hours:.2f} h")
        # unsaved work: the longest stretch without a save, an open (the project on disk replaces the edits) or an
        # autosave write, after the first of them. The autosave writes PRJ-030's interval after the first unsaved
        # change, so the gap is that interval + the time to the first edit after a save or an open
        events = sorted(set([r["autosave"] for r in mon if r["autosave"] > 0] + log.save_times + log.open_times))
        if events and (gap_gate or any(r["autosave"] > 0 for r in mon)):
            first = events[0]
            stamps = [first] + [e for e in events if e > first] + [mon[-1]["wall"] if "wall" in mon[-1] else events[-1]]
            gap = max(b - a for a, b in zip(stamps, stamps[1:])) if len(stamps) > 1 else 0.0
            c.add("unsaved-work gap", (gap <= UNSAVED_GAP_MAX_S) if gap_gate else None, f"{gap:.0f} s",
                  f"<= {UNSAVED_GAP_MAX_S:g} s (PRJ-030 5 min while playing + 60 s to the first edit)")
    if manifest:
        n_s, n_l, n_r = len(manifest.get("saves", [])), len(manifest.get("loads", [])), len(manifest.get("renders", []))
        c.add("saves ok", not log.saves_failed and len(log.saves_ok) >= n_s, f"{len(log.saves_ok)} ok, "
              f"{len(log.saves_failed)} failed", f"{n_s} scripted, no FAILED")
        c.add("loads", log.opens >= n_l and max(0, log.loaded - 1) >= n_l,
              f"{log.opens} OPEN PROJECT screens, {max(0, log.loaded - 1)} projects loaded", f"{n_l} scripted")
        c.add("renders done", log.render_failed == 0 and log.render_done >= n_r,
              f"{log.render_done} done, {log.render_failed} failed", f"{n_r} scripted")
        c.add("song-mode commands", log.arr_bad == 0 and log.arr_ok >= 5 * len(manifest.get("songs", [])),
              f"{log.arr_ok} ok, {log.arr_bad} refused", f"{len(manifest.get('songs', []))} song phases")
        c.add("state checkpoints", log.states >= manifest.get("states", 0) // 2, log.states, "logged")
    if audio is not None:
        with audio.lock:
            secs = audio.frames / max(1, audio.rate)
            tail = list(audio.recent)[-int(TAIL_S * 10):]
            c.add("audio stream", audio.error is None and secs > 0, f"{secs:.0f} s of MAIN",
                  audio.error or "FIFO read to the end")
            c.add("the set sounded", dbfs(audio.peak_all) > ACTIVE_MIN_DBFS, f"{dbfs(audio.peak_all):.1f} dBFS",
                  f"> {ACTIVE_MIN_DBFS:g}")
            tail_pk = max(tail) if tail else 0
            c.add("no stuck notes (last 3 s)", dbfs(tail_pk) <= TAIL_MAX_DBFS, f"{dbfs(tail_pk):.1f} dBFS",
                  f"<= {TAIL_MAX_DBFS:g} after STOP, STOP")
            c.add("longest digital silence", None, f"{audio.longest_silent / 10:.1f} s",
                  "reported (stops and pauses are scripted)")
    return c


# --- the desktop run --------------------------------------------------------------------------------------------------
def find_app_pid(exe, marker, timeout=30.0):
    real = os.path.realpath(exe)
    deadline = time.time() + timeout
    while time.time() < deadline:
        for d in os.listdir("/proc"):
            if not d.isdigit():
                continue
            try:
                if os.path.realpath(f"/proc/{d}/exe") != real:
                    continue
                with open(f"/proc/{d}/environ", "rb") as f:
                    if marker.encode() in f.read():
                        return int(d)
            except OSError:
                continue
        time.sleep(0.2)
    return None


def app_env(ids, d, render, script, audio, kit=True, extra=None):
    p = ids["PROJECT_ENV_PREFIX"]
    env = dict(os.environ)
    env.pop("WAYLAND_DISPLAY", None)
    env.update({"HOME": f"{d}/home", "XDG_CONFIG_HOME": f"{d}/config", "XDG_CACHE_HOME": f"{d}/cache",
                "LIBGL_ALWAYS_SOFTWARE": "1", "EGL_PLATFORM": "x11",
                f"{p}_PLATFORM": "linux", f"{p}_DATA_DIR": f"{d}/data", f"{p}_AUDIO_DEVICE": f"file:{audio}",
                f"{p}_RENDER": render, f"{p}_COMPARE": "0", f"{p}_INPUT_SCRIPT": script})
    mesa = "/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    if "__EGL_VENDOR_LIBRARY_FILENAMES" not in env and os.path.exists(mesa):
        env["__EGL_VENDOR_LIBRARY_FILENAMES"] = mesa
    if kit:
        env[f"{p}_DEV_KIT"] = "Samples/TestKit"
        env[f"{p}_DEV_KIT_SCENES"] = "2"
    for k, v in (extra or {}).items():
        env[f"{p}_{k}"] = v
    for k in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        os.makedirs(env[k], exist_ok=True)
    return env


class App:
    """One app process under xvfb-run, its log pumped into a file and a LogState."""

    def __init__(self, exe, env, log_path, marker, log_state=None, echo=None):
        self.log = log_state or LogState()
        self.log_file = open(log_path, "w")
        self.proc = subprocess.Popen(["xvfb-run", "-a", "-s", "-screen 0 800x480x24", exe], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        self.echo = echo
        self.pump = threading.Thread(target=self._pump, daemon=True)
        self.pump.start()
        self.pid = find_app_pid(exe, marker)

    def _pump(self):
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace")
            self.log_file.write(line)
            self.log.feed(line)
            if self.echo:
                self.echo(line)
        self.log_file.flush()

    def alive(self):
        return self.proc.poll() is None

    def kill(self, sig):
        if self.pid is not None:
            try:
                os.kill(self.pid, sig)   # the app by its exact PID, never by name
            except ProcessLookupError:
                pass

    def wait(self, timeout):
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill(signal.SIGTERM)
            try:
                code = self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.kill(signal.SIGKILL)
                code = self.proc.wait()
            code = code if code else 124
        self.pump.join(timeout=10)
        self.log_file.close()
        return code


def summary_line(t, mon, log, audio, base_rss):
    s = mon[-1] if mon else None
    with log.lock:
        ld = log.loads[-1] if log.loads else None
        p95 = max((x[1] for x in log.loads[-60:]), default=0)
        mx = max((x[2] for x in log.loads[-60:]), default=0)
        saves = f"{len(log.saves_ok)}/{len(log.saves_ok) + len(log.saves_failed)}"
        rend = f"{log.render_done}/{log.render_start}"
        opens = max(0, log.loaded - 1)
    pk = ""
    if audio is not None:
        with audio.lock:
            pk = f" peak={dbfs(audio.peak_window):.1f}dBFS"
            audio.peak_window = 0
    rss = f"rss={s['rss_kb'] / 1024:.1f}MB d={((s['rss_kb'] - base_rss) / 1024 if base_rss else 0):+.1f}MB " \
          f"hwm={s['hwm_kb'] / 1024:.1f}MB fds={s['fds']} thr={s['threads']}" if s else "rss=n/a"
    load = (f"xruns={ld[4]} overruns={ld[3]} p95={p95:.0f} max={mx:.0f}" if ld else "xruns=? (no LOAD yet)")
    hh, rem = divmod(int(t), 3600)
    return (f"SOAK t={hh:02d}:{rem // 60:02d}:{rem % 60:02d} {rss} {load} saves={saves} loads={opens} "
            f"renders={rend}{pk}")


def kill_cycle(exe, ids, d, render, k, rng, out, check_lines):
    """One SIGKILL around a panel save, then a start that must resume cleanly."""
    p = ids["PROJECT_ENV_PREFIX"]
    save_at = 6000
    lines = ["# soak kill test: edits, a panel save, SIGKILL at a random moment around it",
             "3000 press play", "3050 release play"]
    t = 3300
    for i in range(12):
        lines += [f"{t} turn knob1 {rng.choice((-2, 2)):+d}", f"{t + 60} press pad{1 + i % 16} 100",
                  f"{t + 110} release pad{1 + i % 16}"]
        t += 200
    lines += [f"{save_at} press ext.project", f"{save_at + 50} release ext.project",
              f"{save_at + 400} turn sel -20", f"{save_at + 500} turn sel +2",
              f"{save_at + 650} press sel.push", f"{save_at + 700} release sel.push",
              f"{save_at + 1250} press sel.push", f"{save_at + 1300} release sel.push", "20000 quit"]
    kd = os.path.join(out, f"kill-{k}")
    os.makedirs(kd, exist_ok=True)
    with open(os.path.join(kd, "script.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    env = app_env(ids, d, render, os.path.join(kd, "script.txt"), "/dev/null", kit=False)
    marker = f"{p}_INPUT_SCRIPT={os.path.join(kd, 'script.txt')}"
    app = App(exe, env, os.path.join(kd, "log1.txt"), marker)
    # wait for RUN (script time 0 is the first poll, right after RUN), then kill at save push + [0, 400] ms: a
    # desktop save of the kit project takes tens of ms, so most kills land in or just after it
    t0 = time.time()
    while app.log.run_seen is None and time.time() - t0 < 60 and app.alive():
        time.sleep(0.02)
    delay = (save_at + 1250 + rng.randint(0, 400)) / 1000.0
    if app.log.run_seen is not None:
        time.sleep(max(0.0, app.log.run_seen + delay - time.time()))
    app.kill(signal.SIGKILL)
    app.wait(30)
    killed_after = len(app.log.saves_ok)
    # the restart: it must resume the project (recovering an interrupted save), then a checkpoint
    with open(os.path.join(kd, "script2.txt"), "w") as f:
        f.write("4000 cmd state\n6000 quit\n")
    env2 = app_env(ids, d, render, os.path.join(kd, "script2.txt"), "/dev/null", kit=False)
    app2 = App(exe, env2, os.path.join(kd, "log2.txt"), f"{p}_INPUT_SCRIPT={os.path.join(kd, 'script2.txt')}")
    code = app2.wait(90)
    resumed = [x for x in app2.log.project_lines if x.startswith("PROJECT resumed ")]
    leftovers = []
    proj = os.path.join(d, "data", "Projects")
    for name in sorted(os.listdir(proj)) if os.path.isdir(proj) else []:
        if re.search(r"\.tmp-|\.new$|\.bak$|\.commit$", name):
            leftovers.append(name)
    ok = code == 0 and resumed and not leftovers and app2.log.states >= 1 and not app2.log.fatal
    check_lines.append(f"kill {k}: SIGKILL {delay:.2f} s after RUN (save pushed at {(save_at + 1250) / 1000:.2f} s; "
                       f"saves logged before the kill {killed_after}) -> restart exit={code} "
                       f"{resumed[0] if resumed else 'NOT RESUMED: ' + '; '.join(app2.log.project_lines)} "
                       f"leftovers={leftovers or 'none'} state={app2.log.states} -> {'PASS' if ok else 'FAIL'}")
    return bool(ok)


def run_cmd(a):
    ids = identity()
    exe = os.path.join(ROOT, "build", a.preset, "bin", ids["PROJECT_NAME"])
    if not os.access(exe, os.X_OK):
        print(f"no app at {exe}: cmake --preset {a.preset} && cmake --build --preset {a.preset}", file=sys.stderr)
        return 2
    out = os.path.abspath(a.out or os.path.join(ROOT, "out", "soak", datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    os.makedirs(os.path.join(out, "bin"), exist_ok=True)
    # a private copy: a rebuild during a long soak must not replace the binary under the run (or its kill test)
    copy = os.path.join(out, "bin", os.path.basename(exe))
    shutil.copy2(exe, copy)
    exe = copy
    d = out
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen-test-kit.py"), "--data-dir", f"{d}/data",
                    "--json", f"{d}/kit.json"], check=True, stdout=subprocess.DEVNULL)
    script, manifest_path, manifest = generate(out, a.hours, a.seed, a.save_min, a.render_min, a.song_min,
                                               parse_gap(a.gap_ms))
    soak_txt = open(os.path.join(out, "soak.txt"), "w")

    def say(text):
        print(text, flush=True)
        soak_txt.write(text + "\n")
        soak_txt.flush()

    say(f"soak: {out} hours={a.hours:g} seed={a.seed} events={manifest['events']} saves={len(manifest['saves'])} "
        f"loads={len(manifest['loads'])} renders={len(manifest['renders'])} songs={len(manifest['songs'])} "
        f"render={a.render} preset={a.preset}")
    fifo = os.path.join(out, "main.fifo")
    if os.path.exists(fifo):
        os.unlink(fifo)
    os.mkfifo(fifo)
    audio = AudioState()
    reader = threading.Thread(target=audio.read, args=(fifo,), daemon=True)
    reader.start()
    env = app_env(ids, d, a.render, script, fifo)
    heap_csv = None
    if a.heap_stats:
        shim = os.path.join(out, "bin", "heapstat.so")
        subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-o", shim, os.path.join(HERE, "heapstat.c"), "-lpthread"],
                       check=True)
        heap_csv = os.path.join(out, "heap.csv")
        env["LD_PRELOAD"] = shim + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
        env["SOAK_HEAPSTAT_OUT"] = heap_csv
        if a.heap_trim:
            env["SOAK_HEAPSTAT_TRIM"] = "1"
    p = ids["PROJECT_ENV_PREFIX"]
    app = App(exe, env, os.path.join(out, "log.txt"), f"{p}_INPUT_SCRIPT={script}")
    if app.pid is None:
        say("FAIL: the app's PID was not found")
        app.kill(signal.SIGTERM)
        return 1
    say(f"app pid {app.pid}")
    autosave_dir = os.path.join(d, "data", "System", "autosave")
    mon = []
    mon_f = open(os.path.join(out, "monitor.csv"), "w")
    mon_f.write(MON_HEADER + "\n")
    start = time.time()
    limit = manifest["quit_ms"] / 1000.0 + 180
    next_sample, next_summary = start, start + a.summary_min * 60
    base_rss = None
    while app.alive() and time.time() - start < limit:
        now = time.time()
        if now >= next_sample:
            s = proc_sample(app.pid)
            if s:
                s["t"] = now - start
                s["wall"] = now
                s["autosave"] = newest_mtime(autosave_dir)
                mon.append(s)
                mon_f.write(f"{s['t']:.1f},{s['rss_kb']},{s['hwm_kb']},{s['threads']},{s['fds']},{s['ticks']},"
                            f"{s['autosave']:.1f}\n")
                mon_f.flush()
                if base_rss is None and s["t"] >= min(600.0, limit / 4):
                    base_rss = s["rss_kb"]
            next_sample += a.interval
        if now >= next_summary:
            say(summary_line(now - start, mon, app.log, audio, base_rss))
            next_summary += a.summary_min * 60
        time.sleep(0.25)
    code = app.wait(30)
    reader.join(timeout=30)
    mon_f.close()
    say(summary_line(time.time() - start, mon, app.log, audio, base_rss) + " (end)")
    say("--- checks")
    old = sys.stdout
    checks = Checks()

    class Tee:
        def write(self, s):
            old.write(s)
            soak_txt.write(s)

        def flush(self):
            old.flush()

    sys.stdout = Tee()
    try:
        heap = []
        if heap_csv and os.path.exists(heap_csv):
            for line in open(heap_csv):
                f = line.strip().split(",")
                if len(f) == 6 and int(f[0]) == app.pid:   # Xvfb and xvfb-run carry the preload too
                    heap.append({"wall": float(f[1]), "in_use": int(f[2]), "mmapped": int(f[3]), "arena": int(f[4]),
                                 "free": int(f[5])})
        evaluate(app.log, mon, manifest, code, audio, unit=False, checks=checks, heap=heap)
    finally:
        sys.stdout = old
    if a.kill_cycles > 0:
        say(f"--- kill test ({a.kill_cycles} cycles)")
        rng = random.Random(a.seed)
        lines = []
        ok = all([kill_cycle(exe, ids, d, a.render, k + 1, rng, out, lines) for k in range(a.kill_cycles)])
        for line in lines:
            say("  " + line)
        checks.rows.append(("kill test (SIGKILL around a save, resume)", ok,
                            f"{sum(1 for x in lines if x.endswith('PASS'))}/{len(lines)} cycles", "all resume clean"))
    say("--- summary\n" + checks.table())
    failed = checks.failed()
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump({"hours": a.hours, "seed": a.seed, "exit_code": code, "pass": not failed,
                   "checks": [{"name": n, "ok": o, "value": str(v), "limit": l} for n, o, v, l in checks.rows]},
                  f, indent=1)
    say(f"SOAK RESULT {'PASS' if not failed else 'FAIL'} ({len(failed)} failed) artefacts {out}")
    soak_txt.close()
    try:
        os.unlink(fifo)
    except OSError:
        pass
    return 0 if not failed else 1


def analyze_cmd(a):
    log = LogState()
    with open(a.log, errors="replace") as f:
        for i, line in enumerate(f):
            log.feed(line, wall=0.0)
    log.save_times, log.open_times = [], []   # an offline log has no clock: placed by the LOAD-line cadence below
    mon = read_monitor(a.monitor) if a.monitor else []
    if mon:
        # the unit's monitor has wall-clock autosave stamps and monitor times from its start; save times are unknown
        # in an offline log, so the unsaved-work gap uses the autosave stamps and LOAD-line cadence only
        t0 = mon[0]["t"]
        for r in mon:
            r["wall"] = r["t"]
            r["t"] -= t0
        # saves and opens placed in wall time by the LOAD cadence (the monitor starts with the app: ± a few seconds)
        log.save_times = [t0 + x for x in log.save_est]
        log.open_times = [t0 + x for x in log.open_est]
    manifest = json.load(open(a.manifest)) if a.manifest else None
    print(f"soak analyze: {a.log} ({log.lines} lines){' + ' + a.monitor if a.monitor else ''}")
    # the app's log has no time stamps: saves and opens are placed by the LOAD cadence (± 10 s), so the gap is
    # reported, not gated
    c = evaluate(log, mon, manifest, a.exit_code, None, unit=a.unit, gap_gate=False)
    print(c.table())
    failed = c.failed()
    print(f"SOAK RESULT {'PASS' if not failed else 'FAIL'} ({len(failed)} failed)")
    return 0 if not failed else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(s):
        s.add_argument("--hours", type=float, default=1.0)
        s.add_argument("--seed", type=int, default=1)
        s.add_argument("--save-min", type=float, default=10.0)
        s.add_argument("--render-min", type=float, default=10.0)
        s.add_argument("--song-min", type=float, default=15.0)
        s.add_argument("--gap-ms", default="200,1200", help="random pause between routines, ms (lo,hi)")

    s = sub.add_parser("script")
    common(s)
    s.add_argument("--out", required=True)
    r = sub.add_parser("run")
    common(r)
    r.add_argument("--preset", default="host-fast")
    r.add_argument("--render", default="soft")
    r.add_argument("--out")
    r.add_argument("--interval", type=float, default=5.0, help="seconds between /proc samples")
    r.add_argument("--summary-min", type=float, default=10.0)
    r.add_argument("--kill-cycles", type=int, default=3)
    r.add_argument("--heap-stats", action="store_true", help="preload tools/soak/heapstat.c (leak vs fragmentation)")
    r.add_argument("--heap-trim", action="store_true", help="with --heap-stats: malloc_trim(0) once a minute")
    z = sub.add_parser("analyze")
    z.add_argument("--log", required=True)
    z.add_argument("--monitor")
    z.add_argument("--manifest")
    z.add_argument("--exit-code", type=int)
    z.add_argument("--unit", action="store_true", help="gate LoadMeter overruns and max (the hardware run)")
    a = ap.parse_args()
    return {"script": script_cmd, "run": run_cmd, "analyze": analyze_cmd}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
