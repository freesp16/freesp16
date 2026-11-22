#!/usr/bin/env python3
"""The pinned design files match resources/design/SPEC_VERSION (and the spec, when it is checked out next to the
repository): tools/check-spec-sync.sh."""
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(os.environ.get("APP_SOURCE_DIR", pathlib.Path(__file__).resolve().parents[3]))
r = subprocess.run(["sh", str(root / "tools" / "check-spec-sync.sh")], capture_output=True, text=True)
print(r.stdout + r.stderr)
sys.exit(r.returncode)
