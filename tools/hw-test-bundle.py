#!/usr/bin/env python3
"""The hardware-test bundle (docs/hardware-test.md): everything the first full run on the reference
hardware needs besides the app binary — generated here, never device data, no firmware, keys or captures.

  tools/hw-test-bundle.py make --out DIR [--liveset-minutes 30] [--seed 1604]
      DIR/stick/                      copy its content onto the USB stick's root (next to the launcher's files)
        <name>.conf.example           the stick-root launcher settings: network keys only (see the doc)
        <name>-data/<name>/           = /root/data/<name> for the app on the unit (the launcher's bind mount)
          Samples/TestKit/            the generated 16-sample kit (tools/gen-test-kit.py; dev_kit=Samples/TestKit)
          hw-test/conf/NN-<variant>.conf   app settings variants; use.sh copies one to <name>.conf (the app's own
                                      settings file there, re-read on every start) and restarts the app
          hw-test/scripts/<kind>.txt  scripted input (io/controllers/ScriptedInput.h): selftest, routing, linein,
                                      import, lamps, liveset (30 min), crash
          hw-test/use.sh, report.sh   run on the unit (BusyBox sh): pick a variant / print the lines to send back
      DIR/pc/hwt.sh                   run on the PC: variant switching, reports and the device benches over SSH
      DIR/liveset.json, DIR/kit.json  the live-set manifest (tools/liveset-check.py verify) and the kit manifest
  tools/hw-test-bundle.py script --kind KIND --out FILE [--data-dir DIR] [--minutes N] [--seed N]
      one scripted-input file (the host smoke runs `selftest` and `import` on the desktop, scripts/smoke/96-hwready.sh)
  tools/hw-test-bundle.py verify --kind selftest|import|routing|linein --log LOG [--desktop]
      one line per check on a log of that script's run (from the unit: report.sh output or the stick's log; the
      desktop smoke adds --desktop, where no panel, strip or codec exists); exit 1 when one fails

Script phases are logged as "MARK <phase> t=<s>" (the app's `cmd mark`), so a report shows where a problem started.
Human checks (the strip, the lamps, listening to each jack) are marked "HUMAN:" in the scripts and the doc.
Standard library only."""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import stat
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
TAG = re.compile(r"\[sp16-mod\] \S+ (.*)$")


def identity():
    out = subprocess.run([sys.executable, str(HERE / "identity.py"), "--shell"], capture_output=True, text=True,
                         check=True).stdout
    vals = dict(re.findall(r"export (\w+)='?([^'\n]*)'?", out))
    return vals["PROJECT_NAME"], vals["PROJECT_ENV_PREFIX"]


# ------------------------------------------------------------------------------------------------------- scripts
class Script:
    def __init__(self, header):
        self.ev, self.header = [], header

    def at(self, ms, line):
        self.ev.append((int(ms), line))

    def key(self, ms, name, hold=40, velocity=None):
        self.at(ms, f"press {name}" + (f" {velocity}" if velocity is not None else ""))
        self.at(ms + hold, f"release {name}")

    def cmd(self, ms, text):
        self.at(ms, f"cmd {text}")

    def mark(self, ms, label):
        self.cmd(ms, f"mark {label}")

    def text(self):
        lines = [f"# {h}" for h in self.header]
        lines += [f"{ms} {line}" for ms, line in sorted(self.ev, key=lambda e: e[0])]
        return "\n".join(lines) + "\n"


def build_pattern(s, t):
    """Track 1: FULL steps 1, 5, 9, 13; track 2: 3, 7, 11, 15 (the kit's tones 1 and 2). Returns the next time."""
    s.key(t, "pad1", 60, 100)
    t += 250
    s.cmd(t, "trigger-type full")
    t += 80
    for st in (1, 5, 9, 13):
        s.key(t, f"step{st}")
        t += 90
    s.key(t, "pad2", 60, 100)
    t += 250
    for st in (3, 7, 11, 15):
        s.key(t, f"step{st}")
        t += 90
    return t + 200


def queue_scene(s, t, scene):
    for k in ("scene", f"step{scene}", "step1", "scene"):
        s.key(t, k)
        t += 90
    return t


def script_selftest():
    s = Script(["HW test: unattended self-test (tools/hw-test-bundle.py; docs/hardware-test.md step 5)",
                "needs dev_kit=Samples/TestKit dev_kit_scenes=2; time 0 = the app's first frame; ~70 s",
                "HUMAN: listen on MAIN (and the jacks named by the MARK lines); nothing to press"])
    s.mark(2000, "selftest-begin")
    s.mark(2900, "pads 1-16, 400 ms apart: C major scale (1-8), noise bursts (9-12), chords + chirp (13-16)")
    for i in range(16):
        s.key(3000 + 400 * i, f"pad{i + 1}", 80, 110)
    s.mark(10000, "pattern: track 1 steps 1/5/9/13, track 2 steps 3/7/11/15 at 120 BPM")
    t = build_pattern(s, 10100)
    s.key(13000, "play")
    s.mark(17000, "scene: queue scene 2 (empty pattern: silence after the pass end), then scene 1 again")
    queue_scene(s, 17100, 2)
    queue_scene(s, 21000, 1)
    s.mark(25000, "routing 8CH: track 1 to OUT3-4 (track 2 stays on MAIN)")
    s.cmd(25100, "param routing.assignMode 0 1")
    s.cmd(25200, "param routing.outAssign 0 2")
    s.mark(28000, "routing: track 1 to OUT5-6")
    s.cmd(28100, "param routing.outAssign 0 3")
    s.mark(31000, "routing: track 1 to OUT7-8")
    s.cmd(31100, "param routing.outAssign 0 4")
    s.mark(34000, "routing: back to M + 6CH, track 1 on MAIN")
    s.cmd(34100, "param routing.outAssign 0 0")
    s.cmd(34200, "param routing.assignMode 0 0")
    s.mark(36000, "analog filter model: ACTIVE, low-pass closed, resonance up")
    s.cmd(36100, "param afs.active 0 1")
    s.cmd(36200, "param afs.lpCutoff 0 220")
    s.cmd(36300, "param afs.resonance 0 700")
    s.cmd(39000, "param afs.lpCutoff 0 1023")
    s.cmd(39100, "param afs.resonance 0 0")
    s.cmd(39200, "param afs.active 0 0")
    s.key(41000, "stop")
    s.mark(42000, "resample: 2 bars of MAIN onto track 16, starting with PLAY")
    s.cmd(42100, "param sampling.source 0 3")
    s.cmd(42200, "param sampling.threshold 0 42")
    s.cmd(42300, "param sampling.length 0 2")
    s.cmd(42400, "sample-rec 16 2")
    s.key(43000, "play")
    s.key(49000, "stop")
    s.cmd(49500, "sample-state")
    s.mark(50000, "render: the pattern twice, 16-bit, into RenderedAudio/")
    s.cmd(50100, "render-job pattern repeats=2 bits=16")
    s.mark(58000, "save")
    s.cmd(58100, "save-as hwselftest")
    s.cmd(61000, "state")
    s.cmd(61500, "diag")
    s.mark(62000, "selftest-end")
    s.at(64000, "quit")
    return s.text()


def script_routing():
    s = Script(["HW test: output routing, human-paced: 20 s per output; move the monitor cable / headphones",
                "needs dev_kit=Samples/TestKit; the pattern of the self-test plays all along; ~2 min",
                "HUMAN: at each MARK listen on the jack it names; track 1 = the higher tone on steps 1/5/9/13"])
    t = build_pattern(s, 2000)
    s.key(t + 500, "play")
    phases = [
        (6000, "MAIN: both tracks (M + 6CH, every OUT ASSIGN --); PHONES = MASTER", []),
        (26000, "8CH, track 1 to OUT3-4: OUT3-4 = track 1 only, MAIN = track 2 only",
         ["param routing.assignMode 0 1", "param routing.outAssign 0 2"]),
        (46000, "8CH, track 1 to OUT5-6", ["param routing.outAssign 0 3"]),
        (66000, "8CH, track 1 to OUT7-8", ["param routing.outAssign 0 4"]),
        (86000, "8CH, track 1 to OUT3-4, HP CUE OUT3-4: PHONES = track 1 only",
         ["param routing.outAssign 0 2", "param routing.headphoneCue 0 2"]),
        (106000, "back: M + 6CH, HP CUE MASTER, track 1 --",
         ["param routing.headphoneCue 0 0", "param routing.outAssign 0 0", "param routing.assignMode 0 0"]),
    ]
    for ms, label, cmds in phases:
        s.mark(ms, label)
        for i, c in enumerate(cmds):
            s.cmd(ms + 100 + 100 * i, c)
    s.key(120000, "stop")
    s.cmd(120500, "diag")
    s.at(122000, "quit")
    return s.text()


def script_linein():
    s = Script(["HW test: LINE IN sampling: feed a signal into LINE IN before the MARKs",
                "needs dev_kit=Samples/TestKit; ~40 s",
                "HUMAN: play something into LINE IN (a phone, a synth) from 3 s on; listen to pads 14/15 afterwards"])
    s.mark(3000, "linein stereo: 2 bars onto track 15 (starts at once)")
    s.cmd(3100, "param sampling.source 0 0")
    s.cmd(3200, "param sampling.threshold 0 0")
    s.cmd(3300, "param sampling.length 0 2")
    s.cmd(3400, "sample-rec 15 2")
    s.cmd(9000, "sample-state")
    s.mark(10000, "linein L (mono), REC LENGTH OFF: 3 s onto track 14 (sample-stop ends it)")
    s.cmd(10100, "param sampling.source 0 1")
    s.cmd(10200, "param sampling.length 0 0")
    s.cmd(10300, "sample-rec 14 1")
    s.cmd(13300, "sample-stop 14")
    s.cmd(14000, "sample-state")
    s.mark(16000, "playback: pad 15 then pad 14, three times each")
    for i in range(3):
        s.key(16500 + 1500 * i, "pad15", 80, 110)
        s.key(21500 + 1500 * i, "pad14", 80, 110)
    s.cmd(27000, "save-as hwlinein")
    s.cmd(30000, "diag")
    s.at(32000, "quit")
    return s.text()


def script_import():
    s = Script(["HW test: import of the unit's own project from the internal disk; nothing is written there",
                "~45 s; the project is opened from internal_disk's Projects folder (first one A->Z); samples in place",
                "HUMAN: none (listen: the project's scene 1 pattern 1 plays from 14 s to 24 s)"])
    s.mark(2000, "import: list the internal disk's projects")
    s.cmd(2100, "import-sp16 internal:")
    s.mark(3000, "import: the first project, samples in place (read from the internal disk)")
    s.cmd(3100, "import-sp16 internal:1")
    s.cmd(13000, "state")
    s.mark(14000, "play the imported project")
    s.key(14100, "play")
    s.key(24000, "stop")
    s.cmd(25000, "render-job pattern repeats=1 bits=16")
    s.mark(33000, "save the import as a project of this app ($Lib/Projects)")
    s.cmd(33100, "save-as hwimport")
    s.cmd(36000, "diag")
    s.at(38000, "quit")
    return s.text()


def script_lamps():
    s = Script(["HW test: lamps and strip LEDs (UI-150...161) — HUMAN: watch the panel; lamp_log=1 logs every change",
                "~60 s; the keys toggle modes, so the lamps follow; the strip positions light the strip LEDs"])
    t = 3000
    s.mark(t - 100, "pad modes: TRACK, MUTE, SLICE, SCALE, TRACK (one lit at a time)")
    for k in ("PadModeTrack", "PadModeMute", "PadModeSlice", "PadModeScale", "PadModeTrack"):
        s.key(t, k)
        t += 1500
    s.mark(t, "SCENE mode on / off, PATTERN mode on / off")
    for k in ("Scene", "Scene", "Pattern", "Pattern"):
        t += 1500
        s.key(t, k)
    t += 2000
    s.mark(t, "REC on / off; STRIP HOLD on / off; AFS ACTIVE on / off")
    for k in ("Rec", "Rec", "StripHold", "StripHold", "AfsActive", "AfsActive"):
        t += 1500
        s.key(t, k)
    t += 2000
    s.mark(t, "strip LEDs: a finger sweeping 0 -> 127 -> 0 (scripted), then lifted")
    for i, v in enumerate(list(range(0, 128, 16)) + [127] + list(range(112, -1, -16))):
        s.at(t + 300 * i, f"strip {v}")
    t += 300 * 18 + 500
    s.at(t, "strip off")
    s.mark(t + 1000, "pads: every pad lamp in turn (TRACK mode)")
    for i in range(16):
        s.key(t + 1500 + 300 * i, f"pad{i + 1}", 120, 90)
    t += 1500 + 300 * 16 + 1000
    s.mark(t, "lamps-end")
    s.at(t + 2000, "quit")
    return s.text()


def script_crash():
    s = Script(["HW test: crash on purpose after 5 s to check the CRASH report in the log (and that the launcher",
                "brings the stock app back). Use once; then `ctl start` / the next variant."])
    s.mark(4000, "crash-test: SIGSEGV next")
    s.cmd(5000, "diag-crash segv")
    s.at(15000, "quit")
    return s.text()


def script_liveset(minutes, seed, out, manifest):
    subprocess.run([sys.executable, str(HERE / "liveset-check.py"), "script", "--out", str(out), "--manifest",
                    str(manifest), "--minutes", str(minutes), "--seed", str(seed), "--device"], check=True)


KINDS = {"selftest": script_selftest, "routing": script_routing, "linein": script_linein, "import": script_import,
         "lamps": script_lamps, "crash": script_crash}


# ------------------------------------------------------------------------------------------------------- confs
def confs(data, kit=True):
    scripts = f"{data}/hw-test/scripts"
    # gl-fb first, the CPU rasteriser if it fails (an explicit list does not fall back further: FATAL no renderer)
    base = ["render=gl-fb,soft", "diag_interval=10", "input_trace=1"]
    k = ["dev_kit=Samples/TestKit", "dev_kit_scenes=2"]
    return {
        "00-defaults": (["(empty: the app's defaults, render=auto)"], []),
        "01-baseline": (["the GL framebuffer backend (soft if it fails), panel input traced, the 440 Hz test tone on"],
                        base + ["test_tone=1"]),
        "02-soft": (["the CPU rasteriser (when gl-fb fails or for comparison)"], ["render=soft", "diag_interval=10",
                                                                                  "input_trace=1"]),
        "03-audio-off": (["no audio device (isolates display / input problems from the codec)"],
                         ["render=gl-fb,soft", "audio=0", "diag_interval=10", "input_trace=1"]),
        "04-trace": (["every panel / strip event and every lamp change logged (HUMAN: press everything)"],
                     base + ["lamp_log=1", "diag_interval=5"]),
        "05-selftest": (["the unattended self-test on the generated kit (quits after ~65 s)"],
                        base + k + [f"input_script={scripts}/selftest.txt", "diag_interval=5"]),
        "06-routing": (["output routing, 20 s per output (HUMAN: listen on each jack)"],
                       base + k + [f"input_script={scripts}/routing.txt", "diag_interval=5"]),
        "07-linein": (["LINE IN sampling (HUMAN: a signal into LINE IN)"],
                      base + k + [f"input_script={scripts}/linein.txt", "diag_interval=5"]),
        "08-import": (["import the unit's own first project from the internal disk (read only)"],
                      base + [f"input_script={scripts}/import.txt", "diag_interval=5"]),
        "09-lamps": (["lamps + strip LEDs cycled by a script, every change logged (HUMAN: watch the panel)"],
                     base + k + ["lamp_log=1", f"input_script={scripts}/lamps.txt"]),
        "10-liveset": (["the scripted live set (default 30 min) on the kit, scenes 1-2, lamps logged"],
                       base + ["dev_kit=Samples/TestKit", "dev_kit_scenes=2", "lamp_log=1",
                               f"input_script={scripts}/liveset.txt"]),
        "11-period512": (["troubleshooting xruns: 512-frame periods (default 256 x 3)"],
                         base + ["audio_period=512", "audio_periods=3"]),
        "13-filter-loop": (["the analog filter loop on hw:ak4452audio,1 (LP CUTOFF / RESONANCE; ACTIVE on to hear it)"],
                           base + ["filter_loop=1", "diag_interval=5"]),
        "14-phones-s16": (["PHONES as the 16-bit stream of the first builds (if S24 misbehaves)"],
                          base + ["phones_format=s16"]),
        "12-crash": (["checks the CRASH report: crashes on purpose after 5 s"],
                     base + [f"input_script={scripts}/crash.txt"]),
    }


USE_SH = r"""#!/bin/sh
# Run ON THE UNIT (BusyBox sh): pick a settings variant for the app and restart it.
#   sh use.sh                 list the variants
#   sh use.sh 05-selftest     copy conf/05-selftest.conf to the app's settings file (@DATA@/@NAME@.conf, which
#                             the app re-reads on every start) and `ctl restart`
#   sh use.sh none            remove that file (the app's defaults; the stick-root launcher settings still apply)
STAGE=/tmp/@NAME@
LOG=$(cat "$STAGE/log.path" 2>/dev/null)
[ -n "$LOG" ] || { echo "no session log path in $STAGE/log.path (is the launcher running?)" >&2; exit 1; }
USB=$(dirname "$LOG")
DATA="$USB/@NAME@-data/@NAME@"
CONF="$DATA/@NAME@.conf"
v=$1
if [ -z "$v" ]; then
  echo "variants (in $DATA/hw-test/conf):"; ls "$DATA/hw-test/conf" | sed 's/\.conf$//; s/^/  /'
  echo "current: $( [ -f "$CONF" ] && sed -n 's/^# variant //p' "$CONF" || echo none)"; exit 0
fi
if [ "$v" = none ]; then rm -f "$CONF"
else
  [ -f "$DATA/hw-test/conf/$v.conf" ] || { echo "no variant $v (sh use.sh lists them)" >&2; exit 2; }
  cp "$DATA/hw-test/conf/$v.conf" "$CONF" || exit 1
fi
sync
echo "=== hw-test: variant $v ($(date 2>/dev/null))" >> "$LOG"
ctl restart
"""

REPORT_SH = r"""#!/bin/sh
# Run ON THE UNIT (BusyBox sh): print the lines to send back from the app's last run (from its last START line):
# the launcher's lines of that run (from the previous run's end: holders, launch, exit code), START / PLATFORM / SETTINGS / every DIAG line / PROBE / AUDIO / RUN / LOAD /
# FRAME / MARK / CMD / PARAM / KIT / SAMPLE / SAMPLING / RENDER / IMPORT / SAVED / M2 / PANEL / STRIP / LAMP /
# FATAL / CRASH (+ its backtrace) / STOP. INPUT lines are summarised (count per source) unless `all` is given.
#   sh report.sh [all] [runs]      runs = how many app starts back (default 1 = the last)
STAGE=/tmp/@NAME@
LOG=$(cat "$STAGE/log.path" 2>/dev/null)
[ -n "$LOG" ] || { echo "no session log path in $STAGE/log.path" >&2; exit 1; }
ALL=0; RUNS=1
for a in "$@"; do case "$a" in all) ALL=1;; *[!0-9]*) ;; *) RUNS=$a;; esac; done
total=$(grep -c "@PREFIX@ START " "$LOG")
skip=$((total - RUNS))
[ "$skip" -lt 0 ] && skip=0
echo "# report.sh: $LOG, app runs in the log: $total, showing the last $RUNS"
awk -v skip="$skip" -v all="$ALL" -v tag="@PREFIX@ " '
  /runner: / { if (n > skip) print; else pend = pend $0 "\n"; next }
  index($0, tag "START ") { n++; if (n > skip) printf "%s", pend; pend = "" }
  n <= skip { next }
  index($0, tag) == 0 { if (crash) print; next }
  {
    line = substr($0, index($0, tag) + length(tag))
    if (line ~ /^CRASH/) crash = 1
    if (line ~ /^INPUT /) { split(line, f, " "); inputs[f[2]]++; if (!all) next }
    if (line ~ /^(START|PLATFORM|SETTINGS|DIAG|PROBE|COMPARE|AUDIO|RUN|LOAD|FRAME|PACE|MARK|CMD|PARAM|KIT|SAMPLE|SAMPLING|RENDER|IMPORT|SAVED|PROJECT|M2 |LIVE|PANEL|STRIP|SCRIPT|LAMP|FATAL|CRASH|STOP|INPUT|LIBRARY|UI OPEN|ATLAS|PANEL|NORMALIZE|ANALYZE)/) print line
  }
  END { for (s in inputs) printf "INPUT summary source=%s events=%d\n", s, inputs[s] }
' "$LOG"
"""

HWT_SH = r"""#!/bin/sh
# Run ON THE PC (generated by tools/hw-test-bundle.py): the hardware-test steps over SSH (docs/hardware-test.md).
#   pc/hwt.sh use <variant>        switch the app's settings variant on the unit and restart it (`use` alone: list)
#   pc/hwt.sh report [name] [all]  the last run's lines -> @OUT@/reports/<name>-<time>.txt (and the terminal)
#   pc/hwt.sh follow               the live log (Ctrl-C ends)
#   pc/hwt.sh log [name] [lines]   the raw log (default the last 200000 lines) -> @OUT@/reports/log-<name>-<time>.txt
#                                  (what tools/liveset-check.py verify and tools/hw-test-bundle.py verify read)
#   pc/hwt.sh bench [catch args]   stop the app, copy out/armel/@NAME@_bench to /tmp on the unit, run it, save the
#                                  output in @OUT@/reports/bench-<time>.txt, print tools/bench-report.py's table, start
#                                  the app again
#   pc/hwt.sh deploy               push out/armel/@NAME@ (`ctl update`, like the launcher's host/sp16-deploy.sh)
# Environment: SP16_IP (default 169.254.16.16), SP16_PORT (22), SP16_KEY (~/.ssh/id_ed25519) — the launcher's names.
set -eu
REPO=@REPO@
OUT=@OUT@
IP=${SP16_IP:-169.254.16.16}
ssh_unit() {
  ssh -p "${SP16_PORT:-22}" -i "${SP16_KEY:-$HOME/.ssh/id_ed25519}" -o UserKnownHostsFile="$HOME/.ssh/known_hosts_sp16" \
      -o StrictHostKeyChecking=accept-new "root@$IP" "$@"
}
DATA='$(dirname "$(cat /tmp/@NAME@/log.path)")/@NAME@-data/@NAME@'
stamp=$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT/reports"
case "${1:-help}" in
  use) ssh_unit "sh \"$DATA/hw-test/use.sh\" ${2:-}" ;;
  report)
    f="$OUT/reports/${2:-run}-$stamp.txt"
    ssh_unit "sh \"$DATA/hw-test/report.sh\" ${3:-}" > "$f"
    cat "$f"; echo "saved: $f" ;;
  follow) ssh_unit ctl follow ;;
  log)
    f="$OUT/reports/log-${2:-run}-$stamp.txt"
    ssh_unit "ctl log ${3:-200000}" > "$f"
    echo "saved: $f ($(wc -l < "$f") lines)" ;;
  deploy) ssh_unit ctl update < "$REPO/out/armel/@NAME@" ;;
  bench)
    shift
    bin="$REPO/out/armel/@NAME@_bench"
    [ -f "$bin" ] || { echo "no $bin: scripts/build-armel-bench.sh" >&2; exit 1; }
    f="$OUT/reports/bench-$stamp.txt"
    ssh_unit ctl stop || true
    ssh_unit 'cat > /tmp/@NAME@_bench && chmod 0755 /tmp/@NAME@_bench' < "$bin"
    set +e
    ssh_unit "/tmp/@NAME@_bench $*" > "$f" 2>&1
    rc=$?
    set -e
    ssh_unit 'rm -f /tmp/@NAME@_bench; ctl start' || true
    python3 "$REPO/tools/bench-report.py" "$f" || true
    echo "bench exit code $rc; raw output: $f" ;;
  *) sed -n '2,14p' "$0" ;;
esac
"""

README = """Hardware-test bundle (generated by tools/hw-test-bundle.py; the steps are docs/hardware-test.md in the repo).

stick/   copy its CONTENT onto the root of the USB stick that carries the launcher (autoexec image):
           @NAME@-data/@NAME@/Samples/TestKit     the generated test kit (16 synthetic one-shots, no device data)
           @NAME@-data/@NAME@/hw-test/            settings variants (conf/), input scripts (scripts/), use.sh, report.sh
           @NAME@.conf.example                     the launcher's stick-root settings: network keys only. Keep app keys
                                                   out of the stick-root @NAME@.conf: the variants set them, and the
                                                   stick-root file's keys (exported as @PREFIX@_<KEY>) would win.
pc/hwt.sh  run on the PC: `pc/hwt.sh use 05-selftest`, `pc/hwt.sh report selftest`, `pc/hwt.sh bench`.
liveset.json  the live set's manifest: tools/liveset-check.py verify --log <report or log> --manifest liveset.json
              --script stick/@NAME@-data/@NAME@/hw-test/scripts/liveset.txt
Nothing here is firmware, a key, a capture or a copy of device data.
"""

STICK_CONF = """# The launcher's stick-root settings (rename to @NAME@.conf on the stick if you need one). Network keys only:
# the app's keys belong in @NAME@-data/@NAME@/@NAME@.conf (hw-test/use.sh writes it), because these lines are
# exported as @PREFIX@_<KEY> variables, which the app reads too and which win over its own settings file.
ssh=1
ssh_port=22
ssh_ip=169.254.16.16
ssh_netmask=255.255.0.0
ssh_dhcp=0
"""


def subst(text, **kv):
    for k, v in kv.items():
        text = text.replace(f"@{k}@", v)
    return text


def write(path, text, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable:
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def make_cmd(a):
    name, prefix = identity()
    out = pathlib.Path(a.out).resolve()
    stick = out / "stick"
    data_rel = stick / f"{name}-data" / name
    unit_data = f"/root/data/{name}"  # the app's data dir on the unit (the launcher's bind mount of the stick folder)
    subprocess.run([sys.executable, str(HERE / "gen-test-kit.py"), "--data-dir", str(data_rel), "--json",
                    str(out / "kit.json")], check=True, stdout=subprocess.DEVNULL)
    scripts = data_rel / "hw-test" / "scripts"
    for kind, fn in KINDS.items():
        write(scripts / f"{kind}.txt", fn())
    script_liveset(a.liveset_minutes, a.seed, scripts / "liveset.txt", out / "liveset.json")
    for variant, (comment, lines) in confs(unit_data).items():
        body = [f"# variant {variant}", *[f"# {c}" for c in comment],
                "# written by tools/hw-test-bundle.py; hw-test/use.sh copies it to the app's settings file", *lines]
        write(data_rel / "hw-test" / "conf" / f"{variant}.conf", "\n".join(body) + "\n")
    kv = dict(NAME=name, PREFIX=prefix, DATA=unit_data, OUT=str(out), REPO=str(REPO))
    write(data_rel / "hw-test" / "use.sh", subst(USE_SH, **kv), True)
    write(data_rel / "hw-test" / "report.sh", subst(REPORT_SH, **kv), True)
    write(out / "pc" / "hwt.sh", subst(HWT_SH, **kv), True)
    write(out / "README.txt", subst(README, **kv))
    write(stick / f"{name}.conf.example", subst(STICK_CONF, **kv))
    files = sorted(p for p in out.rglob("*") if p.is_file())
    size = sum(p.stat().st_size for p in files)
    print(f"hw-test bundle: {out} ({len(files)} files, {size / 1e6:.1f} MB); variants: "
          + ", ".join(confs(unit_data)))
    return 0


def script_cmd(a):
    if a.kind == "liveset":
        script_liveset(a.minutes, a.seed, a.out, a.manifest or (str(a.out) + ".json"))
        return 0
    text = KINDS[a.kind]()
    if a.data_dir:  # the desktop smoke: the unit's data path → the run's own
        name, _ = identity()
        text = text.replace(f"/root/data/{name}", a.data_dir)
    write(pathlib.Path(a.out), text)
    return 0


# ------------------------------------------------------------------------------------------------------- verify
def app_lines(path):
    out = []
    with open(path, errors="replace") as f:
        for raw in f:
            m = TAG.search(raw.rstrip("\n"))
            if m:
                out.append(m.group(1))
            elif raw.startswith(("START ", "DIAG ", "MARK ", "STOP", "SETTINGS ", "AUDIO ", "LOAD ", "KIT ",
                                 "SAMPLING ", "RENDER ", "SAVED ", "IMPORT ", "M2 ", "PARAM ", "FATAL", "CRASH",
                                 "RUN ", "PLATFORM ", "CMD ", "PROJECT ")):
                out.append(raw.rstrip("\n"))  # a report.sh output (the tag already stripped)
    return out


def verify_cmd(a):
    lines = app_lines(a.log)
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    def has(prefix):
        return any(l.startswith(prefix) for l in lines)

    def count(prefix):
        return sum(1 for l in lines if l.startswith(prefix))

    check(has("START "), "START")
    check(has("SETTINGS "), "one SETTINGS line")
    for d in ("DIAG platform ", "DIAG kernel ", "DIAG cpu ", "DIAG libc ", "DIAG memory ", "DIAG data dir=",
              "DIAG display backend=", "DIAG audio ", "DIAG input ", "DIAG END", "DIAG STOP "):
        check(has(d), f"{d.strip()} line")
    check(not has("FATAL"), "no FATAL")
    check(not has("CRASH"), "no CRASH")
    check(has("STOP"), "STOP")
    xr = [int(m.group(1)) for l in lines if l.startswith(("AUDIO alsa_periods", "LOAD "))
          for m in [re.search(r"xruns=(\d+)", l)] if m]
    check(xr and max(xr) == 0, f"xruns 0 in every AUDIO / LOAD line ({len(xr)} lines, max {max(xr) if xr else '-'})")
    marks = [l for l in lines if l.startswith("MARK ")]
    if a.kind == "selftest":
        check(has("KIT ready"), "KIT ready (the generated kit on the tracks)")
        check(not any(l.startswith("SAMPLE") and "error" in l.lower() for l in lines), "no SAMPLE error")
        for m in ("selftest-begin", "pads 1-16", "pattern:", "scene:", "routing 8CH", "analog filter", "resample:",
                  "render:", "save", "selftest-end"):
            check(any(l.startswith("MARK " + m) for l in marks), f"MARK {m}")
        posted = count("PARAM ")
        refused = sum(1 for l in lines if l.startswith("PARAM ") and "REFUSED" in l)
        check(posted >= 15 and refused == 0, f"PARAM lines posted ({posted}, refused {refused})")
        check(any(l.startswith("M2 SWITCH scene=2") for l in lines), "M2 SWITCH scene=2 (the queued scene switch)")
        check(has("SAMPLING TAKE ok"), "SAMPLING TAKE ok (the resample take)")
        check(has("RENDER JOB done"), "RENDER JOB done")
        check(any(l.startswith("SAVED hwselftest") and " ok" in l for l in lines), "SAVED hwselftest ok")
    elif a.kind == "import":
        check(has("IMPORT unit projects="), "IMPORT unit projects= (the list)")
        check(any(l.startswith("IMPORT done ok") for l in lines), "IMPORT done ok")
        check(has("RENDER JOB done"), "RENDER JOB done (the imported pattern)")
        check(any(l.startswith("SAVED hwimport") and " ok" in l for l in lines), "SAVED hwimport ok")
    elif a.kind == "routing":
        check(len(marks) >= 6, f"6 routing MARK lines ({len(marks)})")
        check(count("PARAM ") >= 8 and not any("REFUSED" in l for l in lines if l.startswith("PARAM ")),
              "PARAM lines posted")
    elif a.kind == "linein":
        check(count("SAMPLING TAKE ok") >= 2, f"two LINE IN takes ({count('SAMPLING TAKE ok')})")
    if not a.desktop:
        check(any(l.startswith("DIAG input ") and " ok " in l for l in lines), "an input source started")
        check(any(l.startswith("DIAG audio device=alsa-direct ok") for l in lines), "DIAG audio: the codec (alsa-direct)")
    for ok, what in results:
        print(("OK    " if ok else "FAIL  ") + what)
    failed = sum(1 for ok, _ in results if not ok)
    print(f"{len(results) - failed}/{len(results)} checks passed" + ("" if not failed else f", {failed} FAILED"))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--out", required=True)
    m.add_argument("--liveset-minutes", type=int, default=30)
    m.add_argument("--seed", type=int, default=1604)
    s = sub.add_parser("script")
    s.add_argument("--kind", required=True, choices=sorted([*KINDS, "liveset"]))
    s.add_argument("--out", required=True)
    s.add_argument("--data-dir")
    s.add_argument("--minutes", type=int, default=30)
    s.add_argument("--seed", type=int, default=1604)
    s.add_argument("--manifest")
    v = sub.add_parser("verify")
    v.add_argument("--kind", required=True, choices=["selftest", "import", "routing", "linein", "any"])
    v.add_argument("--log", required=True)
    v.add_argument("--desktop", action="store_true")
    a = ap.parse_args()
    return {"make": make_cmd, "script": script_cmd, "verify": verify_cmd}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
