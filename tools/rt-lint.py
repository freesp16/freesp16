#!/usr/bin/env python3
"""Real-time lint (NFR-020/021; spec 01-architecture/threading.md: the audio thread
never allocates, locks, blocks, logs or touches files).

  tools/rt-lint.py [files...]     default: every file compiled REALTIME (src/engine/**, see cmake/Modules.cmake)

Flags, in code (comments and string literals are ignored): new, delete (not `= delete`), malloc/calloc/realloc/free,
std::make_unique/make_shared, push_back/emplace_back/resize/insert/reserve, std::string (not std::string_view),
std::function, std::mutex/lock_guard/unique_lock/scoped_lock/condition_variable, printf-family output, std::cout/cerr,
fopen/fwrite/ofstream/ifstream, juce::String, throw. A line that needs one of them (message-thread set-up code living in
an engine file, start-up registration) carries `// rt-ok: <reason>`; a reason is mandatory.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REALTIME_DIRS = ("src/engine/",)  # modules built with app_add_module(... REALTIME)
RULES = [
    (re.compile(r"\bnew\b"), "operator new"),
    (re.compile(r"\bdelete\b"), "operator delete"),  # `= delete` (deleted functions) is exempt, see main()
    (re.compile(r"\b(malloc|calloc|realloc|free)\s*\("), "C allocation"),
    (re.compile(r"\bmake_(unique|shared)\b"), "allocation (make_unique/make_shared)"),
    (re.compile(r"\.\s*(push_back|emplace_back|resize|insert|reserve|emplace)\s*\("), "container growth"),
    (re.compile(r"\bstd::(string|wstring)\b(?!_view)"), "std::string"),
    (re.compile(r"\bstd::function\b"), "std::function"),
    (re.compile(r"\bstd::(mutex|recursive_mutex|shared_mutex|condition_variable|lock_guard|unique_lock|scoped_lock)\b"), "lock"),
    (re.compile(r"\b(printf|fprintf|puts|fputs|perror)\s*\(|\bstd::(cout|cerr|clog)\b"), "logging / console I/O"),
    (re.compile(r"\b(fopen|fwrite|fread|fclose)\s*\(|\bstd::(ofstream|ifstream|fstream)\b"), "file I/O"),
    (re.compile(r"\bjuce::String\b"), "juce::String"),
    (re.compile(r"\bthrow\b"), "throw"),
]
OK = re.compile(r"//\s*rt-ok:\s*\S")


def strip(line: str, state: dict) -> str:
    """Removes comments and string/char literals from one line (block-comment state carried across lines)."""
    out, i, n = [], 0, len(line)
    while i < n:
        if state["block"]:
            j = line.find("*/", i)
            if j < 0:
                return "".join(out)
            state["block"], i = False, j + 2
            continue
        c = line[i]
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            state["block"], i = True, i + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and line[j] != c:
                j += 2 if line[j] == "\\" else 1
            out.append(" ")
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def realtime_files() -> list:
    try:
        files = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "src/engine"], cwd=ROOT, capture_output=True,
                               text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        files = [str(p.relative_to(ROOT)) for p in (ROOT / "src" / "engine").rglob("*")]
    return [f for f in files if f.startswith(REALTIME_DIRS) and f.endswith((".h", ".hpp", ".cpp")) and (ROOT / f).is_file()]


def main() -> int:
    files = sys.argv[1:] or realtime_files()
    problems, lines = [], 0
    for f in files:
        state = {"block": False}
        path = ROOT / f if not pathlib.Path(f).is_absolute() else pathlib.Path(f)
        for number, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            lines += 1
            code = strip(raw, state)
            if not code.strip():
                continue
            hits = []
            for rule, what in RULES:
                if what == "operator delete":
                    if re.search(r"\bdelete\b", code) and not re.search(r"=\s*delete\b", code):
                        hits.append(what)
                elif rule.search(code):
                    hits.append(what)
            if hits and not OK.search(raw):
                problems.append(f"{f}:{number}: {', '.join(hits)}: {raw.strip()}")
    for p in problems:
        print("rt-lint:", p)
    print(f"rt-lint: {len(files)} real-time files, {lines} lines, {len(problems)} findings "
          "(add `// rt-ok: <reason>` only for message-thread code)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
