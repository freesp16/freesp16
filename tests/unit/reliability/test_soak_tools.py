#!/usr/bin/env python3
"""The soak tools themselves (no app run; the soak is run by hand or by scripts/release-check.sh --soak-hours).
  - tools/soak/soak-run.py script: deterministic per seed, sorted times, the soak's own routines on their periods,
    `cmd` only for checkpoints, renders and song mode
  - tools/soak/soak-run.py analyze: a synthetic healthy run passes; growing RSS, an xrun, a failed save, a REALTIME
    VIOLATION each fail it
  - tools/soak/soak-unit.sh (POSIX sh, as on the unit's BusyBox): monitors a stand-in process into the CSV analyze reads
  - tools/soak/check-glibc.py: the host binary (x86-64, glibc > 2.13, dynamic libstdc++) is refused
Exit 77 = skipped (nothing to skip here except check-glibc without readelf / the app)."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

SRC = os.environ.get("APP_SOURCE_DIR") or os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
RUN = os.path.join(SRC, "tools", "soak", "soak-run.py")
UNIT = os.path.join(SRC, "tools", "soak", "soak-unit.sh")
GLIBC = os.path.join(SRC, "tools", "soak", "check-glibc.py")
failures = []


def check(cond, text):
    print(("PASS " if cond else "FAIL ") + text)
    if not cond:
        failures.append(text)


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


tmp = tempfile.mkdtemp(prefix="soak-tools-")
try:
    # --- the script ------------------------------------------------------------------------------------------------
    a, b, c = (os.path.join(tmp, x) for x in ("a", "b", "c"))
    for d, seed in ((a, 7), (b, 7), (c, 8)):
        r = run([sys.executable, RUN, "script", "--out", d, "--hours", "1", "--seed", str(seed)])
        check(r.returncode == 0, f"script --seed {seed}: exit {r.returncode} {r.stderr.strip()[-200:]}")
    digest = lambda d: hashlib.sha256(open(os.path.join(d, "script.txt"), "rb").read()).hexdigest()
    check(digest(a) == digest(b), "the same seed gives the same script")
    check(digest(a) != digest(c), "another seed gives another script")
    m = json.load(open(os.path.join(a, "manifest.json")))
    lines = [x for x in open(os.path.join(a, "script.txt")).read().splitlines() if x and not x.startswith("#")]
    times = [int(x.split(" ", 1)[0]) for x in lines]
    check(times == sorted(times), "times are sorted")
    check(len(lines) == m["events"], f"{len(lines)} steps = the manifest's {m['events']}")
    check(lines[-1].endswith(" quit") and times[-1] >= 3600000, f"ends with quit after 1 h ({times[-1]} ms)")
    # 1 h: cycle 1 (save, load, render) + one per 10 min; renders offset by 5 min; song every 15 min
    check(len(m["saves"]) >= 6 and len(m["loads"]) >= 5, f"saves {len(m['saves'])}, loads {len(m['loads'])}")
    check(len(m["renders"]) >= 6, f"renders {len(m['renders'])}")
    check(len(m["songs"]) >= 2, f"song phases {len(m['songs'])}")
    allowed = re.compile(r"cmd (state|ribbon|render-job pattern bars=1 totrack=0 normalize=0|arr-(clear|append [0-9 ]+|"
                         r"play [01]|state)|param arrangement\.loop 0 1)$")
    bad = [x for x in lines if " cmd " in x and not allowed.search(x)]
    check(not bad, f"`cmd` only for checkpoints, renders and song mode ({bad[:3]})")
    saves = m["save_times"]
    check(all(t2 - t1 >= 15000 for t1, t2 in zip(saves, saves[1:])), "saves >= 15 s apart")
    for ld in m["loads"]:
        prev = max(t for t in saves if t <= ld)
        check(ld - prev >= 30000, f"load at {ld} ms is >= 30 s after its save ({prev} ms)")
        break

    # --- analyze ------------------------------------------------------------------------------------------------------
    def fake_run(name, rss_growth_kb=0, xrun=False, failed_save=False, rt=False):
        d = os.path.join(tmp, name)
        os.makedirs(d)
        tag = "[sp16-mod] APP "
        log = [tag + "SCRIPT ok x: 10 steps over 1000 ms (time 0 = first poll)", tag + "RUN backend=soft"]
        mon = ["t_s,rss_kb,hwm_kb,threads,fds,ticks,autosave_mtime"]
        for i in range(0, 721):   # 2 h, every 10 s
            t = 1795000000 + i * 10
            rss = 100000 + (rss_growth_kb * i // 720)
            mon.append(f"{t},{rss},{rss + 500},7,7,{i * 3},{t - 30 if i % 20 == 0 else ''}")
            x = 1 if xrun and i == 400 else 0
            log.append(tag + f"LOAD cycles/block avg=5000 p95=20000 max=60000 load=1.0% overruns=0 periods={i * 1700} "
                             f"xruns={x} VmRSS={rss} kB")
            if i % 60 == 0:
                log.append(tag + ("SAVED Untitled FAILED disk full" if failed_save and i == 120 else "SAVED Untitled ok "))
        if rt:
            log.append("REALTIME VIOLATION: malloc on the audio thread")
        log.append(tag + "STOP")
        open(os.path.join(d, "log.txt"), "w").write("\n".join(log) + "\n")
        open(os.path.join(d, "monitor.csv"), "w").write("\n".join(mon) + "\n")
        return d

    def analyze(d, *extra):
        return run([sys.executable, RUN, "analyze", "--log", os.path.join(d, "log.txt"), "--monitor",
                    os.path.join(d, "monitor.csv"), "--exit-code", "0", *extra])

    r = analyze(fake_run("healthy", rss_growth_kb=3000))
    check(r.returncode == 0 and "SOAK RESULT PASS" in r.stdout, "a healthy 2 h run passes" + ("" if r.returncode == 0
          else ":\n" + r.stdout[-1500:]))
    r = analyze(fake_run("leak", rss_growth_kb=40000))
    check(r.returncode == 1 and re.search(r"RSS growth after warm-up\s+FAIL", r.stdout), "40 MB of growth fails")
    r = analyze(fake_run("xrun", xrun=True))
    check(r.returncode == 1 and re.search(r"xruns\s+FAIL", r.stdout), "an xrun fails")
    r = analyze(fake_run("rt", rt=True))
    check(r.returncode == 1 and re.search(r"REALTIME VIOLATION\s+FAIL", r.stdout), "a REALTIME VIOLATION fails")
    d = fake_run("save")
    man = os.path.join(d, "manifest.json")
    json.dump({"events": 10, "saves": ["save-1", "save-2"], "loads": [], "renders": [], "songs": [], "states": 0},
              open(man, "w"))
    r = analyze(d, "--manifest", man)
    check(r.returncode == 0, "the manifest's counts are met" + ("" if r.returncode == 0 else ":\n" + r.stdout[-1200:]))
    d = fake_run("savefail", failed_save=True)
    json.dump({"events": 10, "saves": ["save-1"], "loads": [], "renders": [], "songs": [], "states": 0},
              open(os.path.join(d, "manifest.json"), "w"))
    r = analyze(d, "--manifest", os.path.join(d, "manifest.json"))
    check(r.returncode == 1 and re.search(r"saves ok\s+FAIL", r.stdout), "a failed save fails")

    # --- the unit's monitor -------------------------------------------------------------------------------------------
    sh = shutil.which("busybox")
    shell = [sh, "sh"] if sh else ["sh"]
    u = os.path.join(tmp, "unit")
    stand_in = ("i=0; while [ $i -lt 4 ]; do echo '[sp16-mod] APP LOAD cycles/block avg=1 p95=2 max=3 load=0.1% "
                "overruns=0 periods=10 xruns=0 VmRSS=1000 kB'; i=$((i+1)); sleep 1; done")
    r = run(shell + [UNIT, "--out", u, "--interval", "1", "--summary", "2", "--", "sh", "-c", stand_in], timeout=60)
    check(r.returncode == 0, f"soak-unit.sh ({' '.join(shell)}): exit {r.returncode} {r.stderr.strip()[-300:]}")
    check("SOAK t=00:00:02" in r.stdout and "p95=2" in r.stdout, "a SOAK line with the app's last LOAD line")
    rows = open(os.path.join(u, "monitor.csv")).read().splitlines()
    check(rows[0] == "t_s,rss_kb,hwm_kb,threads,fds,ticks,autosave_mtime" and len(rows) >= 4,
          f"monitor.csv: header + {len(rows) - 1} samples")
    check(all(re.fullmatch(r"\d+,\d+,\d+,\d+,\d+,\d+,\d*", x) for x in rows[1:]), "monitor rows are numeric")
    r = run([sys.executable, RUN, "analyze", "--log", os.path.join(u, "log.txt"), "--monitor",
             os.path.join(u, "monitor.csv")])
    check("RSS growth after warm-up" in r.stdout and "open fds flat" in r.stdout, "analyze reads the unit's files")

    # --- check-glibc --------------------------------------------------------------------------------------------------
    exe = os.environ.get("APP_EXECUTABLE", "")
    if shutil.which("readelf") and exe and os.path.exists(exe):
        r = run([sys.executable, GLIBC, exe])
        check(r.returncode == 1 and "ARM expected" in r.stdout and "newer than 2.13" in r.stdout,
              "check-glibc refuses the host binary (x86-64, glibc > 2.13)")
    else:
        print("SKIP check-glibc (no readelf or no host app)")
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print(f"{'OK' if not failures else 'FAILED'}: {len(failures)} failure(s)")
sys.exit(1 if failures else 0)
