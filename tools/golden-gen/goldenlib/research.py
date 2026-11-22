"""Locate the research models and import them behind a guard (no firmware-derived inputs).

Allowed research inputs: `docs/` and the Python models `tools/fx/*.py` only. The guard enforces that for everything
imported through it:
  - only top-level `tools/fx/<module>.py` files are served; `tools/fx/verify/` (and any package below tools/fx) is
    refused, and tools/fx is never put on sys.path;
  - the loaders that read firmware-derived tables from the user's own files — `device_out.load_table` (OutputVolume
    table) and `glibc_expf.load_tables` (the libm `expf` tables) — are replaced by stubs that raise
    `ForbiddenInput`, so no golden this tool produces can depend on them;
  - every served file is recorded with its SHA-256 and licence (MIT / LGPL-2.1-or-later / fdlibm notice), and the case
    .json lists them.
The research checkout is found at $APP_RESEARCH_DIR, else next to this repository, else next to the main checkout of a
git worktree (`../PioneerSP16FirmwareResearch`). The spec pins commit 8901c970a277b45fd7e12967d533b69dcbd0588c.
"""
import builtins
import hashlib
import importlib.abc
import importlib.util
import os
import pathlib
import re
import sys

PINNED_COMMIT = "8901c970a277b45fd7e12967d533b69dcbd0588c"
REPO_NAME = "PioneerSP16FirmwareResearch"
FORBIDDEN_LOADERS = {"device_out": ("load_table",), "glibc_expf": ("load_tables",)}


class ForbiddenInput(RuntimeError):
    """A model tried to read a firmware-derived table; goldens never depend on those."""


class ResearchMissing(RuntimeError):
    pass


def _main_checkout(root):
    """For a git worktree (`.git` is a file 'gitdir: <main>/.git/worktrees/<id>'), the main checkout's folder."""
    git = root / ".git"
    if git.is_file():
        m = re.match(r"gitdir:\s*(.+)", git.read_text().strip())
        if m:
            p = pathlib.Path(m.group(1))
            for parent in p.parents:
                if parent.name == ".git":
                    return parent.parent
    return None


def find_research(repo_root, explicit=None):
    candidates = []
    if explicit:
        candidates.append(pathlib.Path(explicit))
    if os.environ.get("APP_RESEARCH_DIR"):
        candidates.append(pathlib.Path(os.environ["APP_RESEARCH_DIR"]))
    candidates.append(repo_root.parent / REPO_NAME)
    main = _main_checkout(repo_root)
    if main is not None:
        candidates.append(main.parent / REPO_NAME)
    for c in candidates:
        if (c / "tools" / "fx").is_dir():
            return c.resolve()
    raise ResearchMissing("research models not found (tried: %s); set APP_RESEARCH_DIR or check out %s at %s next "
                          "to the repository" % (", ".join(str(c) for c in candidates), REPO_NAME, PINNED_COMMIT[:7]))


def licence_of(text):
    head = "\n".join(text.splitlines()[:12])
    if "SPDX-License-Identifier: LGPL-2.1-or-later" in head:
        return "LGPL-2.1-or-later"
    if "Sun Microsystems" in head:
        return "MIT + fdlibm notice (Sun Microsystems)"
    if "MIT" in head:
        return "MIT"
    return "unknown"


class _Stub:
    def __init__(self, module, name):
        self.what = "%s.%s" % (module, name)

    def __call__(self, *a, **k):
        raise ForbiddenInput("%s reads a firmware-derived table and must never be called by golden-gen; "
                             "this case needs a local-only golden (cases.json \"local_only\")" % self.what)


def _refuse_open(file, *a, **k):
    raise ForbiddenInput("a research model tried to open %r: golden-gen models never read files" % (file,))


class _ResearchFinder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    def __init__(self, fx_dir):
        self.fx = fx_dir
        self.served = {}                   # module name -> {"file", "licence", "sha256"}

    def find_spec(self, name, path=None, target=None):
        if name == "lief" or name.startswith("lief."):
            raise ForbiddenInput("golden-gen never imports lief: it is only used to read firmware-derived tables")
        if "." in name:
            return None
        if name == "verify" or (self.fx / name).is_dir():
            raise ImportError("golden-gen never imports research packages such as tools/fx/%s/" % name)
        f = self.fx / (name + ".py")
        if not f.is_file():
            return None
        return importlib.util.spec_from_file_location(name, str(f), loader=self)

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        f = pathlib.Path(module.__spec__.origin)
        src = f.read_bytes()
        code = compile(src, str(f), "exec", dont_inherit=True)
        exec(code, module.__dict__)
        for attr in FORBIDDEN_LOADERS.get(module.__name__, ()):
            if hasattr(module, attr):
                setattr(module, attr, _Stub(module.__name__, attr))
        self.served[module.__name__] = {
            "file": "tools/fx/%s" % f.name,
            "licence": licence_of(src.decode("utf-8", "replace")),
            "sha256": hashlib.sha256(src).hexdigest(),
        }


class Research:
    """One guarded view of the research models. `load(name)` imports tools/fx/<name>.py (and what it imports)."""

    def __init__(self, root):
        self.root = pathlib.Path(root)
        self.fx = self.root / "tools" / "fx"
        self._finder = None

    def __enter__(self):
        for name in list(sys.modules):     # a fresh model state per golden (models keep module-level tables)
            if name in self._model_names():
                del sys.modules[name]
        self._finder = _ResearchFinder(self.fx)
        sys.meta_path.insert(0, self._finder)
        sys.dont_write_bytecode = True     # never write __pycache__ into the research checkout
        self._open = builtins.open
        builtins.open = _refuse_open
        return self

    def __exit__(self, *exc):
        builtins.open = self._open
        sys.meta_path.remove(self._finder)
        for name in self._finder.served:
            sys.modules.pop(name, None)
        return False

    def _model_names(self):
        return {p.stem for p in self.fx.glob("*.py")}

    def load(self, name):
        if name not in self._model_names():
            raise ImportError("tools/fx/%s.py does not exist" % name)
        return importlib.import_module(name)

    def served(self):
        return [dict(v) for _, v in sorted(self._finder.served.items())]
