#!/usr/bin/env python3
"""tools/gen_theme.py is strict (UID-050): the pinned tokens generate with the app's palettes; an unknown token in a
known group, a missing token the UI uses, an unknown top-level group, or an app palette that replaces a spec palette
fails the build."""
import copy
import json
import os
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(os.environ.get("APP_SOURCE_DIR", pathlib.Path(__file__).resolve().parents[3]))
gen = root / "tools" / "gen_theme.py"
tokens = json.loads((root / "resources" / "design" / "tokens.json").read_text(encoding="utf-8"))
app_tokens = json.loads((root / "resources" / "design" / "app-tokens.json").read_text(encoding="utf-8"))


def run(t: dict, app=None) -> subprocess.CompletedProcess:
    app = app_tokens if app is None else app
    with tempfile.TemporaryDirectory() as d:
        src, dst = pathlib.Path(d) / "tokens.json", pathlib.Path(d) / "ThemeTokens.h"
        src.write_text(json.dumps(t), encoding="utf-8")
        extra = []
        if app:
            extra = [pathlib.Path(d) / "app-tokens.json"]
            extra[0].write_text(json.dumps(app), encoding="utf-8")
        r = subprocess.run([sys.executable, str(gen), str(src), str(dst), *map(str, extra)], capture_output=True,
                           text=True)
        r.header = dst.read_text(encoding="utf-8") if dst.exists() else ""
        return r


failures = []
ok = run(tokens)
if ok.returncode != 0 or "elevation2Shadow" not in ok.header or "motionEase" not in ok.header or "light" not in ok.header \
        or "Palette classic" not in ok.header:
    failures.append(f"pinned tokens must generate: {ok.stderr}")
if "classic" in tokens["color"]:
    failures.append("tokens.json has a classic palette: the app's palettes belong in app-tokens.json")
if run(tokens, {}).returncode == 0:
    failures.append("spec tokens without the app palettes: generator accepted it")
a = copy.deepcopy(app_tokens); a["color"]["dark"] = tokens["color"]["dark"]
if run(tokens, a).returncode == 0:
    failures.append("app palette replacing color.dark: generator accepted it")

cases = []
t = copy.deepcopy(tokens); t["color"]["dark"]["surface-9"] = {"$value": "#000000"}; cases.append(("unknown colour", t))
t = copy.deepcopy(tokens); del t["color"]["light"]["accent"]; cases.append(("missing light accent", t))
t = copy.deepcopy(tokens); t["shadow"] = {}; cases.append(("unknown group", t))
t = copy.deepcopy(tokens); del t["motion"]["fast"]; cases.append(("missing motion.fast", t))
t = copy.deepcopy(tokens); t["font"]["style"]["huge"] = {"size": 99, "line": 99, "weight": 400}; cases.append(("unknown style", t))
t = copy.deepcopy(tokens); t["elevation"]["2"]["$value"] = "soft"; cases.append(("bad shadow", t))
for name, case in cases:
    if run(case).returncode == 0:
        failures.append(f"{name}: generator accepted it")

for f in failures:
    print("FAIL", f)
print("gen_theme strictness:", "ok" if not failures else f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
