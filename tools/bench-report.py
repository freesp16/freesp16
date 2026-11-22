#!/usr/bin/env python3
"""Turn the device bench's output (out/armel/<name>_bench run on a unit, tests/device-bench/DeviceBenchMain.cpp) into a
table of every bench against its Cortex-A9 budget. Standard library only.

  tools/bench-report.py OUTPUT.txt [--markdown] [--csv FILE] [--only-over]

Input lines (any prefix before "BENCH" is ignored, so a copy of the app log works too):
  BENCH <name> budget=<cycles> measured=<cycles> (…) p95=<cycles> min=<cycles> … OK|OVER
  BENCHCASE START|PASS|FAIL <test case> …   BENCH DIAG cpu … freq_khz=… governor=…   BENCH DONE rc=…   BENCH CRASH …
One cycle = 1 ns at 1 GHz (CLOCK_MONOTONIC_RAW). The printed budget is the code's; where the code's budget is a
desktop gate rather than the A9 figure, the A9 budget below is used instead (column "A9 budget", note "gate"):
  ui_*                  host gate = A9 budget / 10 (bench_ui_frame's host/A9 ratio, our choice): A9 budget = 10 × gate
  decode_listing_10k    host gate = 2 × the A9 budget (10,000 entries in 300 ms)
Every row also names the budget it answers (column "ref"). Failed Catch2 cases are listed (on the
A9 a case fails whenever its desktop gate is exceeded, so they do not gate). Exit code: 0 = every bench within its A9
budget and the run finished, 1 = something OVER, a crash, or a case that started and never ended, 2 = no BENCH lines."""
import argparse
import csv
import re
import sys

LINE = re.compile(r"BENCH (?P<name>[A-Za-z0-9_]+) budget=(?P<budget>[0-9.]+) measured=(?P<measured>[0-9.]+)"
                  r".*?p95=(?P<p95>[0-9.]+) min=(?P<min>[0-9.]+).*?(?P<status>OK|OVER)\s*$")
CASE = re.compile(r"BENCHCASE (?P<kind>START|PASS|FAIL) (?P<name>.+?)(?: assertions=(?P<assertions>\d+) "
                  r"failed=(?P<failed>\d+) ms=(?P<ms>[0-9.]+))?\s*$")

# name prefix -> what the number answers (the first match wins; most specific first)
REFS = [
    ("bench_mixer", "mixer, 16 strips (45 k)"),
    ("bench_trigger", "trigger routing (6 k)"),
    ("afs_", "analog filter model (45 k)"),
    ("bench_midi", "MIDI"),
    ("midi_", "MIDI"),
    ("bench_stretch", "time-stretch"),
    ("decode_listing", "library listing (300 ms)"),
    ("bench_render_offline", "render >= 1.8x real time (REN)"),
    ("bench_render_", "render service (REN)"),
    ("bench_routing", "output routing"),
    ("bench_sampling", "sampling"),
    ("bench_engine_m1", "engine share, kit playback (181 k)"),
    ("bench_voice", "voices"),
    ("bench_fx", "effects shell"),
    ("fx_", "effects"),
    ("bench_sequencer", "sequencer"),
    ("bench_scene", "scenes"),
    ("bench_edit", "edit / chains"),
    ("bench_perf", "performance features"),
    ("bench_lfo", "LFO"),
    ("bench_ampenv", "amp envelope"),
    ("bench_ribbon", "touch strip"),
    ("bench_slice", "slices"),
    ("bench_arrangement", "arrangement"),
    ("bench_transport", "transport"),
    ("bench_project", "project store"),
    ("store_", "project store"),
    ("model_", "model"),
    ("commands_", "command bus"),
    ("decode_", "decode / pool"),
    ("render_", "UI renderer (soft)"),
    ("ui_", "UI frame (16.7 ms frame)"),
    ("engine_", "engine"),
    ("load_meter", "LoadMeter"),
    ("bench_harness", "harness"),
]


def ref_of(name):
    for prefix, ref in REFS:
        if name.startswith(prefix):
            return ref
    return ""


def a9_budget(name, budget):
    """(A9 budget, note) for a bench whose code budget is a desktop gate."""
    if name.startswith("ui_"):
        return budget * 10.0, "gate x10"
    if name == "decode_listing_10k":
        return 300e6, "gate/2"
    return budget, ""


def human(cycles):
    if cycles >= 1e6:
        return f"{cycles / 1e6:.2f} M"
    if cycles >= 1e3:
        return f"{cycles / 1e3:.1f} k"
    return f"{cycles:.0f}"


def parse(lines):
    benches, cases, diag, done, crash, started = {}, {}, [], None, [], set()
    for raw in lines:
        at = raw.find("BENCH")
        if at < 0:
            continue
        line = raw[at:].rstrip("\n")
        if line.startswith("BENCHCASE"):
            m = CASE.match(line)
            if m:
                if m["kind"] == "START":
                    started.add(m["name"])
                else:
                    cases[m["name"]] = m["kind"]
            continue
        if line.startswith("BENCH DIAG "):
            diag.append(line[len("BENCH DIAG "):])
        elif line.startswith("BENCH DONE"):
            done = line
        elif line.startswith("BENCH CRASH"):
            crash.append(line)
        else:
            m = LINE.match(line)
            if m:
                benches[m["name"]] = {k: (float(v) if k in ("budget", "measured", "p95", "min") else v)
                                      for k, v in m.groupdict().items()}
    unfinished = sorted(started - set(cases))
    return benches, cases, diag, done, crash, unfinished


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("--markdown", action="store_true", help="a Markdown table (default: aligned text)")
    ap.add_argument("--csv", help="also write the rows to this CSV file")
    ap.add_argument("--only-over", action="store_true", help="list only the benches above their A9 budget")
    a = ap.parse_args()
    with open(a.output, errors="replace") as f:
        benches, cases, diag, done, crash, unfinished = parse(f)
    if not benches:
        print(f"no BENCH lines in {a.output}", file=sys.stderr)
        return 2
    rows = []
    for name, b in sorted(benches.items()):
        budget, note = a9_budget(name, b["budget"])
        pct = 100.0 * b["measured"] / budget if budget > 0 else 0.0
        status = "OK" if b["measured"] <= budget else "OVER"
        rows.append({"bench": name, "a9_budget": budget, "measured": b["measured"], "p95": b["p95"], "min": b["min"],
                     "pct": pct, "status": status, "note": note, "ref": ref_of(name)})
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    shown = [r for r in rows if r["status"] == "OVER"] if a.only_over else rows
    for line in diag:
        if line.startswith(("cpu ", "kernel ", "libc ", "memory ")):
            print(f"# {line}")
    head = ["bench", "A9 budget", "measured", "p95", "% budget", "status", "note", "ref"]
    cells = [[r["bench"], human(r["a9_budget"]), human(r["measured"]), human(r["p95"]), f"{r['pct']:.0f}%",
              r["status"], r["note"], r["ref"]] for r in shown]
    if a.markdown:
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join("---" for _ in head) + "|")
        for c in cells:
            print("| " + " | ".join(c) + " |")
    else:
        widths = [max(len(h), *(len(c[i]) for c in cells)) if cells else len(h) for i, h in enumerate(head)]
        print("  ".join(h.ljust(widths[i]) for i, h in enumerate(head)))
        for c in cells:
            print("  ".join(v.ljust(widths[i]) for i, v in enumerate(c)))
    over = [r["bench"] for r in rows if r["status"] == "OVER"]
    failed = sorted(n for n, k in cases.items() if k == "FAIL")
    def names(items, limit=25):
        return ", ".join(items[:limit]) + (f", … {len(items) - limit} more" if len(items) > limit else "")

    print(f"\n{len(rows)} benches: {len(rows) - len(over)} within the A9 budget, {len(over)} OVER"
          + (f" ({names(over)})" if over else ""))
    if failed:
        # A FAIL from a case whose only failed check is a desktop gate is expected on the A9 (see the "note" column).
        print(f"{len(failed)} failed test cases: {names(failed)}")
    if unfinished:
        print(f"started but never finished (crash or hang there): {', '.join(unfinished)}")
    for line in crash[:20]:
        print(line)
    print(done or "no BENCH DONE line: the run did not finish")
    return 0 if not over and not unfinished and done is not None and not crash else 1


if __name__ == "__main__":
    sys.exit(main())
