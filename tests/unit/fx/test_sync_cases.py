#!/usr/bin/env python3
"""The committed SYNC-conversion cases (tests/unit/fx/data/sync_convert.txt) are up to date with the research model
(skipped, 77, without ../PioneerSP16FirmwareResearch)."""
import pathlib
import subprocess
import sys

here = pathlib.Path(__file__).resolve().parent
sys.exit(subprocess.call([sys.executable, "-B", str(here / "sync_cases.py"), "--check"]))
