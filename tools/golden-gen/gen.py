#!/usr/bin/env python3
"""Golden generator (spec 05-quality/testing-strategy.md "DSP goldens from the research models").

  tools/golden-gen/gen.py <target>...        generate (write) goldens
  tools/golden-gen/gen.py --all              every tests/golden/<name>/cases.json
  tools/golden-gen/gen.py --all --check      regenerate in memory and compare bit for bit with the committed files
  tools/golden-gen/gen.py --list             list golden names and cases
  options: --research DIR (else $APP_RESEARCH_DIR, else ../PioneerSP16FirmwareResearch), --local-dir DIR (local-only
           goldens, default build/golden-local, git-ignored)
  <target>: a cases.json path, a golden name (`mixer`) or one case (`mixer/fader_steps`)
Exit status: 0 ok, 1 a golden is stale / an error, 77 the research models are missing (nothing could be checked).

Layout. A golden is a folder `tests/golden/<name>/` with a hand-written `cases.json` (the recipe):
  {"model": "<adapter in goldenlib/models>", "subject": "<C++ subject id>", "local_only": false,
   "sample_rate": 44100, "block": 64, "tolerance": {"max_rel": 1e-5, "residual_dbfs": -100.0, "floor": 1e-30},
   "cases": [{"id": "...", "description": "...", "blocks": N, "params": {...},
              "stimuli": {"<input channel>": <stimulus spec, goldenlib/stimuli.py>},
              "parity": true, "fault_tests": ["gain"], "tolerance": {...overrides}, "window": [first, end]}]}
gen.py writes, per case, `<id>.json` (everything resolved: parameters, the seeded stimulus specs, tolerances, the app
tables plugged into the model, the research files driven with their licence and SHA-256), `<id>.in.f32` (the
generated input) and `<id>.out.f32` (the model's output) — raw float32 LE, channel-major (goldenlib/f32.py). The C++
runner (tests/golden/harness, `ctest -L golden`) reads them; CMake registers one test per case from cases.json.

Committed vs local: goldens are committed only when CI can regenerate them, i.e. the model needs no
firmware-derived input. A recipe with `"local_only": true` (or an adapter with OWNER_INPUTS) is written to the
git-ignored local folder instead, never committed, and its tests skip when the files are missing (CI). This tool
itself never loads a firmware-derived table: research.py stubs those loaders out, and an adapter that needs
user-supplied inputs is reported and skipped.

Dependencies: Python 3.8+ standard library only (the research models use no numpy).
"""
import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True

from goldenlib import f32, research, stimuli  # noqa: E402
from goldenlib.models import ADAPTERS  # noqa: E402

GOLDEN_DIR = ROOT / "tests" / "golden"
FORMAT = "golden-case/1"
DEFAULT_TOLERANCE = {"max_rel": 1e-5, "residual_dbfs": -100.0, "floor": 1e-30}
CASE_KEYS = {"id", "description", "blocks", "params", "stimuli", "parity", "fault_tests", "tolerance", "window"}


class GoldenError(Exception):
    pass


def dumps(obj, indent=0):
    """Pretty JSON with scalar lists on one line (stable, diff-friendly; every sha256 sits on its own line)."""
    pad = "  " * indent
    if isinstance(obj, dict):
        if not obj:
            return "{}"
        if all(not isinstance(v, (dict, list)) for v in obj.values()):
            flat = json.dumps(obj)
            if len(flat) <= 100:
                return flat
        items = ['%s  %s: %s' % (pad, json.dumps(k), dumps(v, indent + 1)) for k, v in obj.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(obj, list):
        if all(not isinstance(v, (dict, list)) for v in obj):
            return "[" + ", ".join(json.dumps(v) for v in obj) + "]"
        return "[\n" + ",\n".join("%s  %s" % (pad, dumps(v, indent + 1)) for v in obj) + "\n" + pad + "]"
    return json.dumps(obj)


def load_recipe(path):
    try:
        r = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        raise GoldenError("%s: %s" % (path, e))
    for key in ("model", "subject", "cases"):
        if key not in r:
            raise GoldenError("%s: missing %r" % (path, key))
    if r["model"] not in ADAPTERS:
        raise GoldenError("%s: unknown model %r (adapters: %s)" % (path, r["model"], ", ".join(sorted(ADAPTERS))))
    if ADAPTERS[r["model"]].SUBJECT != r["subject"]:
        raise GoldenError("%s: model %r drives subject %r, not %r" % (path, r["model"], ADAPTERS[r["model"]].SUBJECT,
                                                                       r["subject"]))
    if r.get("block", 64) != 64:
        raise GoldenError("%s: the engine block is 64 frames (EffectBase contract)" % path)
    ids = [c.get("id") for c in r["cases"]]
    if len(set(ids)) != len(ids) or not all(isinstance(i, str) and i.replace("_", "").isalnum() for i in ids):
        raise GoldenError("%s: case ids must be unique [A-Za-z0-9_]: %r" % (path, ids))
    for c in r["cases"]:
        extra = set(c) - CASE_KEYS
        if extra:
            raise GoldenError("%s: case %s: unknown keys %s" % (path, c["id"], sorted(extra)))
    return r


def is_local(recipe):
    return bool(recipe.get("local_only")) or bool(ADAPTERS[recipe["model"]].OWNER_INPUTS)


def build_case(name, recipe, case, res):
    """-> {filename: bytes} for one case."""
    adapter = ADAPTERS[recipe["model"]]
    block = int(recipe.get("block", 64)); fs = float(recipe.get("sample_rate", 44100))
    blocks = int(case["blocks"]); frames = blocks * block
    case = dict(case); case["_block"] = block
    adapter.validate(case)
    in_names = adapter.input_channels(case)
    specs = case.get("stimuli", {})
    unknown = set(specs) - set(in_names)
    if unknown:
        raise GoldenError("%s/%s: stimuli for unknown inputs %s (inputs: %s)" % (name, case["id"], sorted(unknown),
                                                                                  in_names))
    inputs = [stimuli.generate(specs.get(ch, {"type": "silence"}), frames, fs) for ch in in_names]
    with research.Research(res) if adapter.RESEARCH_MODELS else _NoResearch() as r:
        outputs, tables = adapter.run(case, inputs, r, block)
        models = r.served()
    out_names = adapter.output_channels(case)
    if len(outputs) != len(out_names) or any(len(c) != frames for c in outputs):
        raise GoldenError("%s/%s: model returned %d channels, expected %d x %d frames" %
                          (name, case["id"], len(outputs), len(out_names), frames))
    tol = dict(DEFAULT_TOLERANCE); tol.update(recipe.get("tolerance", {})); tol.update(case.get("tolerance", {}))
    in_bytes = f32.to_bytes(inputs); out_bytes = f32.to_bytes(outputs)
    cid = case["id"]
    doc = {
        "format": FORMAT,
        "name": name,
        "case": cid,
        "description": case.get("description", ""),
        "subject": recipe["subject"],
        "model": recipe["model"],
        "sample_rate": fs,
        "block": block,
        "blocks": blocks,
        "frames": frames,
        "params": case.get("params", {}),
        "tolerance": tol,
        "window": case.get("window", [0, frames]),
        "parity": bool(case.get("parity", False)),
        "fault_tests": case.get("fault_tests", []),
        "input": {"file": cid + ".in.f32", "channels": in_names,
                  "stimuli": {ch: specs.get(ch, {"type": "silence"}) for ch in in_names},
                  "sha256": f32.sha256(in_bytes)},
        "expected": {"file": cid + ".out.f32", "channels": out_names, "sha256": f32.sha256(out_bytes)},
        "tables": tables,
        "models": models,
        "research": {"repository": research.REPO_NAME, "pinned_commit": research.PINNED_COMMIT,
                     "allowed": "docs/ and tools/fx/*.py only"} if adapter.RESEARCH_MODELS else
                    {"reference": "own (tools/golden-gen/goldenlib/models/%s.py)" % recipe["model"]},
        "generator": "tools/golden-gen/gen.py",
    }
    return {cid + ".json": (dumps(doc) + "\n").encode(), cid + ".in.f32": in_bytes, cid + ".out.f32": out_bytes}


class _NoResearch:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def load(self, name):
        raise GoldenError("this adapter declares no research models")

    def served(self):
        return []


def first_difference(a, b):
    if len(a) != len(b):
        return "size %d bytes, regenerated %d bytes" % (len(a), len(b))
    for i in range(0, len(a), 4):
        if a[i:i + 4] != b[i:i + 4]:
            x = f32.from_bytes(a[i:i + 4], 1)[0][0]; y = f32.from_bytes(b[i:i + 4], 1)[0][0]
            return "first difference at sample %d: committed %r, regenerated %r" % (i // 4, x, y)
    return "differs"


def rel(p):
    try:
        return str(pathlib.Path(p).resolve().relative_to(ROOT))
    except ValueError:
        return str(p)


def resolve_targets(args):
    """-> [(name, recipe path, [case ids] or None)]"""
    out = []
    if args.all:
        for p in sorted(GOLDEN_DIR.glob("*/cases.json")):
            out.append((p.parent.name, p, None))
    for t in args.targets:
        p = pathlib.Path(t)
        if p.name == "cases.json" and p.is_file():
            out.append((p.resolve().parent.name, p.resolve(), None))
            continue
        name, _, cid = t.partition("/")
        p = GOLDEN_DIR / name / "cases.json"
        if not p.is_file():
            raise GoldenError("no golden %r (%s missing)" % (name, p))
        out.append((name, p, [cid] if cid else None))
    if not out:
        raise GoldenError("nothing to do: name a target or pass --all")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--check", action="store_true", help="compare with the committed files, write nothing")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--research")
    ap.add_argument("--local-dir", default=str(ROOT / "build" / "golden-local"))
    args = ap.parse_args(argv)
    try:
        targets = resolve_targets(args) if not args.list else [(p.parent.name, p, None)
                                                                for p in sorted(GOLDEN_DIR.glob("*/cases.json"))]
        if args.list:
            for name, path, _ in targets:
                r = load_recipe(path)
                print("%s (%s -> %s%s): %s" % (name, r["model"], r["subject"], ", local only" if is_local(r) else "",
                                               ", ".join(c["id"] for c in r["cases"])))
            return 0
        res = None
        needs_research = any(ADAPTERS[load_recipe(p)["model"]].RESEARCH_MODELS for _, p, _ in targets)
        if needs_research:
            try:
                res = research.find_research(ROOT, args.research)
            except research.ResearchMissing as e:
                print("gen.py: skipped: %s" % e, file=sys.stderr)
                return 77
        stale = 0; done = 0; skipped = 0
        for name, path, only in targets:
            recipe = load_recipe(path)
            adapter = ADAPTERS[recipe["model"]]
            local = is_local(recipe)
            if adapter.OWNER_INPUTS:
                print("%s: skipped: needs user-supplied firmware-derived inputs %s, which golden-gen never loads"
                      % (name, adapter.OWNER_INPUTS))
                skipped += 1
                continue
            if local and args.check:
                print("%s: local-only golden, not checked (never committed)" % name)
                skipped += 1
                continue
            out_dir = (pathlib.Path(args.local_dir) / name) if local else path.parent
            cases = [c for c in recipe["cases"] if only is None or c["id"] in only]
            if only and not cases:
                raise GoldenError("%s: no case %s" % (name, only))
            produced = set()
            for case in cases:
                files = build_case(name, recipe, case, res)
                for fname, data in files.items():
                    produced.add(fname)
                    target = out_dir / fname
                    if args.check:
                        old = target.read_bytes() if target.is_file() else None
                        if old != data:
                            stale += 1
                            why = "missing" if old is None else (first_difference(old, data) if fname.endswith(".f32")
                                                                 else "content differs")
                            print("STALE %s/%s: %s" % (name, fname, why))
                    else:
                        out_dir.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(data)
                done += 1
            if only is None:
                generated = {p.name for p in out_dir.glob("*") if p.name != "cases.json"
                             and (p.suffix == ".f32" or p.suffix == ".json")} if out_dir.is_dir() else set()
                for extra in sorted(generated - produced):
                    if args.check:
                        stale += 1
                        print("STALE %s/%s: not produced by cases.json (remove it or add the case)" % (name, extra))
                    else:
                        (out_dir / extra).unlink()
                        print("%s: removed %s (no longer in cases.json)" % (name, extra))
            print("%s: %d case(s) %s" % (name, len(cases), "checked" if args.check else "written to " + rel(out_dir)))
        if args.check:
            if stale:
                print("gen.py --check: %d file(s) stale — regenerate with tools/golden-gen/gen.py <name> and review "
                      "the diff" % stale)
                return 1
            print("gen.py --check: %d case(s) up to date%s" % (done, (", %d skipped" % skipped) if skipped else ""))
        return 0
    except (GoldenError, research.ForbiddenInput, ValueError, KeyError) as e:
        print("gen.py: error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
