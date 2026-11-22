#!/usr/bin/env python3
"""Hardware-test tools: tools/hw-test-bundle.py make lays out the hardware-test bundle (kit under the stick's
<name>-data/<name>/Samples/TestKit, every settings variant, every script, the helpers), its scripts parse as the
scripted-input format (`<ms> <op> …`, no '#' glued to a word: it would start a comment there), the variants point at
the unit's data path and carry no device-only secret; tools/bench-report.py reads the device bench's lines, applies the
A9 budgets of desktop-gated benches and gates on OVER / an unfinished case / a missing BENCH DONE."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

SRC = pathlib.Path(os.environ.get("APP_SOURCE_DIR", pathlib.Path(__file__).resolve().parents[3]))
OPS = {"press", "release", "turn", "value", "strip", "touch", "drag", "release-touch", "tap", "screenshot", "quit",
       "cmd"}


def identity():
    out = subprocess.run([sys.executable, str(SRC / "tools/identity.py"), "--shell"], capture_output=True, text=True,
                         check=True).stdout
    vals = dict(re.findall(r"export (\w+)='?([^'\n]*)'?", out))
    return vals["PROJECT_NAME"], vals["PROJECT_ENV_PREFIX"]


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    return 0 if ok else 1


def test_bundle(tmp):
    name, _ = identity()
    out = tmp / "hw"
    subprocess.run([sys.executable, str(SRC / "tools/hw-test-bundle.py"), "make", "--out", str(out),
                    "--liveset-minutes", "2"], check=True, stdout=subprocess.DEVNULL)
    data = out / "stick" / f"{name}-data" / name
    fails = 0
    kit = sorted((data / "Samples/TestKit").glob("*.wav"))
    fails += check(len(kit) == 16, f"16 kit samples in the stick's data folder ({len(kit)})")
    for f in ("hw-test/use.sh", "hw-test/report.sh"):
        fails += check(os.access(data / f, os.X_OK), f"{f} executable")
    fails += check(os.access(out / "pc/hwt.sh", os.X_OK), "pc/hwt.sh executable")
    confs = sorted((data / "hw-test/conf").glob("*.conf"))
    fails += check(len(confs) >= 12, f"settings variants ({len(confs)})")
    for c in confs:
        text = c.read_text()
        for line in text.splitlines():
            if line.startswith("#") or not line.strip():
                continue
            key, _, value = line.partition("=")
            fails += check(re.fullmatch(r"[a-z0-9_.]+", key) is not None, f"{c.name}: key '{key}'")
            if key == "input_script":
                rel = value.replace(f"/root/data/{name}/", "")
                fails += check(value.startswith(f"/root/data/{name}/") and (data / rel).is_file(),
                               f"{c.name}: input_script {value} is in the bundle")
    scripts = sorted((data / "hw-test/scripts").glob("*.txt"))
    want = {"selftest", "routing", "linein", "import", "lamps", "crash", "liveset"}
    fails += check(want <= {s.stem for s in scripts}, f"scripts {sorted(want)}")
    for s in scripts:
        bad = []
        for n, line in enumerate(s.read_text().splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split()
            # '#' after a blank starts a comment (intended); glued to a word it would cut a command short
            if not parts[0].isdigit() or parts[1] not in OPS or re.search(r"\S#", line):
                bad.append(f"line {n}: {line}")
        fails += check(not bad, f"{s.name}: every step parses" + (f" ({bad[:3]})" if bad else ""))
    # Nothing device-derived or secret in the bundle.
    for p in out.rglob("*"):
        if p.is_file() and p.suffix in (".bin", ".key", ".upd", ".img"):
            fails += check(False, f"no firmware/key/image files ({p})")
    return fails


def test_bench_report(tmp):
    report = SRC / "tools/bench-report.py"
    out = tmp / "bench.txt"
    out.write_text(
        "BENCH DEVICE START x 0.1 args=(all) scratch=/tmp/x\n"
        "BENCH DIAG cpu online=4 configured=4 model=\"ARMv7 Processor rev 10 (v7l)\" freq_khz=996000 governor=performance\n"
        "BENCHCASE START bench_mixer\n"
        "BENCH bench_mixer budget=45000 measured=41000.0 (91.1% of budget) p95=43000.0 min=40000.0 cycles@1GHz "
        "batch=8 samples=101 OK\n"
        "BENCHCASE PASS bench_mixer assertions=3 failed=0 ms=900\n"
        "BENCHCASE START ui_frame: home\n"
        "[sp16-mod] X BENCH ui_frame_home budget=50000 measured=300000.0 (600.0% of budget) p95=320000.0 "
        "min=290000.0 cycles@1GHz batch=1 samples=1 OVER\n"
        "BENCH ui_frame home: p50 0.3 ms (a free-text line, ignored)\n"
        "BENCHCASE FAIL ui_frame: home assertions=2 failed=1 ms=50\n"
        "BENCH DONE rc=1 cases=2 failed=1 seconds=2\n")
    r = subprocess.run([sys.executable, str(report), str(out)], capture_output=True, text=True)
    fails = check(r.returncode == 0, f"desktop-gated OVER within its A9 budget: exit 0 ({r.returncode})\n{r.stdout}")
    fails += check("ui_frame_home" in r.stdout and "gate x10" in r.stdout, "ui_* rows use 10 x the gate")
    fails += check("mixer, 16 strips" in r.stdout, "bench_mixer row has its label")
    fails += check("2 benches: 2 within the A9 budget, 0 OVER" in r.stdout, "summary line")
    out.write_text(out.read_text().replace("measured=41000.0", "measured=46000.0").replace(" OK\n", " OVER\n", 1)
                   + "BENCHCASE START bench_trigger\n")
    r = subprocess.run([sys.executable, str(report), str(out), "--markdown"], capture_output=True, text=True)
    fails += check(r.returncode == 1, f"OVER and an unfinished case: exit 1 ({r.returncode})")
    fails += check("| bench_mixer |" in r.stdout and "never finished (crash or hang there): bench_trigger" in r.stdout,
                   "markdown table and the unfinished case")
    empty = tmp / "empty.txt"
    empty.write_text("nothing\n")
    r = subprocess.run([sys.executable, str(report), str(empty)], capture_output=True, text=True)
    fails += check(r.returncode == 2, f"no BENCH lines: exit 2 ({r.returncode})")
    return fails


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        fails = test_bundle(tmp) + test_bench_report(tmp)
    print(f"{fails} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
