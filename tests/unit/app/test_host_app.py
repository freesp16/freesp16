#!/usr/bin/env python3
"""The composition root on the host platform: the app selects the host platform,
builds the atlas, compares the CPU backends, starts the engine on the offline audio device, runs `run_frames` HOME
frames through ui::createRoot() and stops cleanly — the same code path as on the device, minus the hardware.
Skipped (77) when the host app was not built (APP_BUILD_HOST_APP=OFF)."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile


def main() -> int:
    exe = os.environ.get("APP_EXECUTABLE", "")
    if not exe or not pathlib.Path(exe).exists():
        print("skipped: host app not built")
        return 77
    sys.path.insert(0, str(pathlib.Path(os.environ["APP_SOURCE_DIR"]) / "tools"))
    import identity  # noqa: E402  (tools/identity.py: the project's name lives in cmake/ProjectIdentity.cmake only)

    prefix = identity.read()["ENV_PREFIX"]
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home, XDG_CACHE_HOME=home)
        env.update({f"{prefix}_PLATFORM": "host", f"{prefix}_RUN_FRAMES": "45", f"{prefix}_COMPARE": "1",
                    f"{prefix}_COMPARE_FRAMES": "30"})
        run = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=120)
    log = [l for l in run.stdout.splitlines() if l.startswith(f"[sp16-mod] {prefix} ")]
    text = "\n".join(log)
    print(text)
    expect = [r" START ", r" PLATFORM host selected", r" ATLAS \d+x\d+ for 800x480", r" FRAME compare backend=soft",
              r" AUDIO ok offline:", r" RUN backend=soft", r" FRAME run backend=soft frames=45",
              r" AUDIO alsa_periods=0 xruns=0 juce_callbacks=0 engine_blocks=[1-9]\d* device=offline", r" STOP$"]
    missing = [e for e in expect if not re.search(e, text, re.M)]
    for e in missing:
        print("MISSING", e)
    if run.returncode != 0:
        print("exit code", run.returncode, run.stderr[-2000:])
    return 0 if not missing and run.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
