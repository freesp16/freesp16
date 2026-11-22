#!/usr/bin/env python3
"""Bench regression check (the CI `bench` job): compare the results of
`ctest --preset host-release -L bench` with ci/bench-baseline.json and fail when a bench got more than the baseline's
tolerance (10 %) slower on the same machine.

  tests/bench/harness/check_regression.py [--results DIR] [--baseline FILE] [--machine KEY] [--update]

  --results   folder of <name>.json results (default build/host-release/artifacts/bench; written by every BENCH and
              every bench::record of tests/bench)
  --baseline  default ci/bench-baseline.json
  --machine   baseline key (default: <os>-<arch>-<cpu model>, e.g. "darwin-arm64-Apple M1 (Virtual)"); numbers only
              compare on the machine that recorded them, so a machine without a baseline reports "new" and passes
  --update    write the results as this machine's baseline (keeps other machines), then exit 0

The compared metric is the baseline's "metric" (min_cycles: the fastest batch, the least noisy figure on a shared or
busy host; the budget check of each bench uses the median). Repeated runs into the same results folder keep each
bench's fastest result (ci/run-local.sh bench runs the suite 3 times). "tolerance_overrides" gives single benches a
wider tolerance (file-system bound ones: fsync and cache timings are not CPU regressions). Exit 0 = no regression, 1 = regression, 2 = no results.
"""
import argparse
import json
import pathlib
import platform
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]


def cpu_model() -> str:
    system = platform.system()
    try:
        if system == "Darwin":
            return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True,
                                  check=True).stdout.strip()
        if system == "Linux":
            text = pathlib.Path("/proc/cpuinfo").read_text(errors="replace")
            m = re.search(r"^(model name|Model|Hardware)\s*:\s*(.+)$", text, re.M)
            if m:
                return m.group(2).strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    return platform.processor() or "unknown"


def machine_key() -> str:
    return f"{platform.system().lower()}-{platform.machine()}-{cpu_model()}"


def load_results(folder: pathlib.Path) -> dict:
    out = {}
    for f in sorted(folder.glob("*.json")):
        try:
            r = json.loads(f.read_text())
            out[r["name"]] = r
        except (ValueError, KeyError) as e:
            print(f"bench-check: ignoring {f}: {e}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--results", default=str(ROOT / "build/host-release/artifacts/bench"))
    ap.add_argument("--baseline", default=str(ROOT / "ci/bench-baseline.json"))
    ap.add_argument("--machine", default=None)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()

    results = load_results(pathlib.Path(args.results))
    if not results:
        print(f"bench-check: no results in {args.results} (run `ctest --preset host-release -L bench` first)")
        return 2
    path = pathlib.Path(args.baseline)
    baseline = json.loads(path.read_text()) if path.exists() else {}
    baseline.setdefault("schema", 1)
    baseline.setdefault("metric", "min_cycles")
    baseline.setdefault("tolerance", 0.10)
    baseline.setdefault("machines", {})
    baseline.setdefault("tolerance_overrides", {})
    metric, tolerance = baseline["metric"], float(baseline["tolerance"])
    overrides = baseline["tolerance_overrides"]
    key = args.machine or machine_key()

    if args.update:
        baseline["machines"][key] = {n: round(float(r[metric]), 1) for n, r in sorted(results.items())}
        path.write_text(json.dumps(baseline, indent=2, sort_keys=False) + "\n")
        print(f"bench-check: baseline for '{key}' updated with {len(results)} benches ({path})")
        return 0

    base = baseline["machines"].get(key, {})
    print(f"bench-check: machine '{key}', metric {metric}, tolerance {tolerance:.0%}"
          + ("" if base else " — no baseline for this machine: every bench is new (record one with --update)"))
    print(f"  {'bench':<40} {'budget':>12} {'median':>12} {metric:>12} {'baseline':>12} {'change':>8}  status")
    regressions = 0
    for name, r in sorted(results.items()):
        cur = float(r[metric])
        ref = base.get(name)
        if ref is None:
            change, status = "", "new"
        else:
            ratio = cur / ref if ref > 0 else 1.0
            change = f"{(ratio - 1) * 100:+.1f}%"
            limit = float(overrides.get(name, tolerance))
            status = "REGRESSION" if ratio > 1 + limit else "ok"
            if limit != tolerance:
                status += f" (tolerance {limit:.0%})"
            regressions += status.startswith("REGRESSION")
        over = " OVER-BUDGET" if float(r["median_cycles"]) > float(r["budget_cycles"]) else ""
        print(f"  {name:<40} {r['budget_cycles']:>12.0f} {r['median_cycles']:>12.1f} {cur:>12.1f} "
              f"{'' if ref is None else f'{ref:.1f}':>12} {change:>8}  {status}{over}")
    for name in sorted(set(base) - set(results)):
        print(f"  {name:<40} (in the baseline, no result: not built or renamed)")
    print(f"bench-check: {len(results)} benches, {regressions} regression(s) > {tolerance:.0%}")
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
