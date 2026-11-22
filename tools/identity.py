#!/usr/bin/env python3
"""Read cmake/ProjectIdentity.cmake (the single source of the project's name) for scripts.

  tools/identity.py                 print KEY=value lines (NAME, DISPLAY_NAME, ID, VERSION, ENV_PREFIX)
  tools/identity.py NAME            print one value
  eval "$(tools/identity.py --shell)"   export PROJECT_NAME, PROJECT_DISPLAY_NAME, ... into a shell
"""
import pathlib
import re
import shlex
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FILE = ROOT / "cmake" / "ProjectIdentity.cmake"
LINE = re.compile(r'^set\(PROJECT_IDENTITY_(?P<key>[A-Z_]+)\s+"(?P<value>[^"]*)"\)\s*$')


def read() -> dict:
    values = {}
    for line in FILE.read_text(encoding="utf-8").splitlines():
        m = LINE.match(line.strip())
        if m:
            values[m["key"]] = m["value"]
    for key in ("NAME", "DISPLAY_NAME", "ID", "VERSION"):
        if key not in values:
            sys.exit(f"{FILE}: missing set(PROJECT_IDENTITY_{key} ...)")
    if not re.fullmatch(r"[a-z][a-z0-9-]*", values["NAME"]):
        sys.exit(f"{FILE}: NAME must be lowercase letters, digits and hyphens")
    values["ENV_PREFIX"] = values["NAME"].upper().replace("-", "_")
    return values


def main() -> int:
    values = read()
    args = sys.argv[1:]
    if args == ["--shell"]:
        for key, value in values.items():
            print(f"export PROJECT_{key}={shlex.quote(value)}")
    elif args:
        print(values[args[0]])
    else:
        for key, value in values.items():
            print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
