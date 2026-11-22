#!/usr/bin/env python3
"""Check the module dependency rules (spec 01-architecture/module-layout.md).

  tools/check-deps.py            exit 1 and list every forbidden #include edge

Parses every `#include "..."` and `#include <juce_*...>` of the tracked C++ files and checks it against the table:

  app ─▶ ui ─▶ services ─▶ core
   │      │       │
   │      └──────▶ engine ─▶ core (model/enums, model/paths, util, params/ParamId only)
   ├────────────▶ io ─────▶ engine/EngineApi.h, core
   └─▶ platform ─▶ io, engine/EngineApi.h, ui/render/{Renderer,RendererEnv}.h, core

  - core includes only core, and no JUCE.
  - engine includes engine, core/util, core/audio (SampleBuffer), core/model/enums, core/model/paths, core/params/ParamId.h — never
    core/model/Project.h (or any other model header), services, ui, io, platform, nor any JUCE GUI module.
  - services includes services, core, engine/EngineApi.h.
  - io includes io, core, engine/EngineApi.h; no JUCE GUI.
  - ui includes ui, services, core, io/controllers (the ControlEvent contract, our choice), engine/EngineApi.h and the
    data-only effect headers engine/fx/EffectDescriptor.h, engine/fx/EffectRegistry.h.
  - platform includes platform, io, core, engine/EngineApi.h, ui/render/Renderer.h, ui/render/RendererEnv.h,
    ui/render/DrawList.h (included by Renderer.h).
  - app (the composition root) includes the public headers of every module but only platform/Platform.h of platform/
    and only engine/EngineApi.h of engine/.
  - nothing outside platform/ and app/ includes platform/** (tests/ may: contract tests and fakes).
Generated headers (ProjectIdentity.h, BinaryData.h, ui/ThemeTokens.h, gldispatch_names.inc) are allowed outside
core/ and engine/.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"  # include root of the modules: "engine/EngineApi.h" = src/engine/EngineApi.h
MODULES = ("core", "engine", "services", "io", "platform", "ui", "app", "tests")
GENERATED = {"ProjectIdentity.h", "BinaryData.h", "ui/ThemeTokens.h", "gldispatch_names.inc"}
JUCE_GUI = re.compile(r"juce_(gui_basics|gui_extra|opengl|graphics)\b")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def allowed(module: str, target: str) -> bool:
    """target: repository-relative path of a project header."""
    top = target.split("/", 1)[0]
    if module == "tests":
        return True
    if module == "core":
        return top == "core"
    if module == "engine":
        return (top == "engine" or target.startswith(("core/util/", "core/audio/", "core/model/enums/", "core/model/paths/"))
                or target == "core/params/ParamId.h")
    if module == "services":
        return top in ("services", "core") or target == "engine/EngineApi.h"
    if module == "io":
        return top in ("io", "core") or target == "engine/EngineApi.h"
    if module == "ui":
        return (top in ("ui", "services", "core") or target.startswith("io/controllers/")
                or target in ("engine/EngineApi.h", "engine/fx/EffectDescriptor.h", "engine/fx/EffectRegistry.h",
                              "engine/fx/EffectRegistryOrder.h", "engine/fx/FxSlotState.h"))  # data-only
    if module == "platform":
        return (top in ("platform", "io", "core") or target == "engine/EngineApi.h"
                or target in ("ui/render/Renderer.h", "ui/render/RendererEnv.h", "ui/render/DrawList.h"))
    if module == "app":
        if top == "platform":
            return target == "platform/Platform.h"
        if top == "engine":
            return target == "engine/EngineApi.h"
        return top in ("app", "core", "services", "io", "ui")
    return True


def tracked_sources() -> list:
    """Module-relative paths ("engine/Engine.cpp" for src/engine/Engine.cpp; "tests/..." stays repository-relative)."""
    try:
        out = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "*.h", "*.hpp", "*.cpp", "*.inc"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        out = [str(p.relative_to(ROOT)) for p in ROOT.rglob("*") if p.suffix in (".h", ".hpp", ".cpp")]
    rel = [f[len("src/"):] if f.startswith("src/") else f for f in out]
    return [f for f in rel if f.split("/", 1)[0] in MODULES and full(f).is_file()]


def full(rel: str) -> pathlib.Path:
    return ROOT / rel if rel.startswith("tests/") else SRC / rel


def resolve(src: str, name: str):
    for base in (SRC, ROOT, full(src).parent):
        candidate = (base / name).resolve()
        if candidate.is_file():
            for root in (SRC, ROOT):
                try:
                    return str(candidate.relative_to(root))
                except ValueError:
                    continue
            return None
    return None


def main() -> int:
    problems, edges = [], 0
    for src in tracked_sources():
        module = src.split("/", 1)[0]
        for number, line in enumerate(full(src).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            kind, name = m.groups()
            if kind == "<":
                if name.startswith("juce_") and module in ("core",):
                    problems.append(f"{src}:{number}: core must not include JUCE ({name})")
                elif JUCE_GUI.match(name) and module in ("engine", "io"):
                    problems.append(f"{src}:{number}: {module} must not include the JUCE GUI module {name}")
                continue
            edges += 1
            target = resolve(src, name)
            if target is None:
                if name in GENERATED or name.endswith("/ThemeTokens.h"):
                    if module in ("core", "engine"):
                        problems.append(f"{src}:{number}: {module} must not include generated {name}")
                    continue
                problems.append(f"{src}:{number}: unresolved include \"{name}\"")
                continue
            if target.split("/", 1)[0] == "platform" and module not in ("platform", "app", "tests"):
                problems.append(f"{src}:{number}: only platform/ and app/ may include platform/** ({target})")
                continue
            if not allowed(module, target):
                problems.append(f"{src}:{number}: {module} must not include {target} (module-layout.md)")
    for p in problems:
        print("check-deps:", p)
    print(f"check-deps: {edges} include edges checked, {len(problems)} violations")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
