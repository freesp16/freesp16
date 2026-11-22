#!/usr/bin/env python3
"""ctest label interop-private (local only, never CI): runs the hidden "[.private]" interop test over your own
files in tests/interop/private/ (git-ignored). Exit 77 (skipped) when that folder holds no .prj / .rsm / .wav.dat.
  ctest --preset host-fast -L interop-private --output-on-failure      (the IMPORT lines are in the output)
Standard library only."""
import os
import pathlib
import subprocess
import sys

src = pathlib.Path(os.environ.get("APP_SOURCE_DIR", pathlib.Path(__file__).resolve().parents[3]))
build = pathlib.Path(os.environ.get("APP_BINARY_DIR", src / "build" / "host-fast"))
private = pathlib.Path(os.environ.get("INTEROP_PRIVATE_DIR", src / "tests" / "interop" / "private"))
files = [p for p in private.rglob("*") if p.is_file() and p.name.lower().endswith((".prj", ".rsm", ".wav.dat"))] \
    if private.is_dir() else []
if not files:
    print(f"skipped: no private files in {private}")
    sys.exit(77)
name = subprocess.run([sys.executable, str(src / "tools" / "identity.py"), "--shell"], capture_output=True,
                      text=True, check=True).stdout
name = next(l.split("=", 1)[1] for l in name.splitlines() if l.startswith("export PROJECT_NAME="))
exe = build / "tests" / f"{name}_tests_interop"
if not exe.exists():
    print(f"no test binary at {exe}")
    sys.exit(1)
sys.exit(subprocess.run([str(exe), "[.private]"], env=dict(os.environ, INTEROP_PRIVATE_DIR=str(private))).returncode)
