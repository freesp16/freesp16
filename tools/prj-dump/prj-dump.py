#!/usr/bin/env python3
"""Print a reference project (.prj, System/data.rsm) or sample sidecar (.wav.dat) — to inspect your
own files locally (never commit them). Independent decoder written from research docs/project-format.md (CC BY 4.0).

  tools/prj-dump/prj-dump.py FILE                tree as indented text (blobs summarised)
  tools/prj-dump/prj-dump.py FILE --json         tree as JSON (typed values, blobs base64)
  tools/prj-dump/prj-dump.py FILE --steps        + every non-empty step cell of every PatternData (locks decoded;
                                                 the matrix is track-major and a programmed record names its own
                                                 track / step: "coords=ok", else MISMATCH)
  tools/prj-dump/prj-dump.py FILE --roundtrip    re-encode (outer zlib 1, minimal ints) and compare byte for byte
  tools/prj-dump/prj-dump.py FILE --names        every node type with its property names and types (no values):
                                                 the part worth sending back to fix the importer's field table
Read-only: the file is opened for reading only. Standard library only.

Check of your own projects (local only; nothing of it is ever committed):
  1. copy your .prj files, System/data.rsm and some .wav.dat into tests/interop/private/ (git-ignored), then
       ctest --preset host-fast -L interop-private --output-on-failure -V
     → per file: "IMPORT …" report lines (content, samples, roundtrip, every enum-unmapped / unknown-prop / opaque-kept
       / guessed line) and the tree round trip; plus this tool's --names and --roundtrip output for one file
  2. in the app (scripted input, see scripts/smoke/94-import.sh): `cmd import-sp16 /absolute/path/X.prj` (add
     ` copy` to copy the samples into the library) with the disk copy in its usual layout (…/PIONEER DJ SAMPLER/
     Projects/X.prj with Samples/ beside Projects/), then play it; the log's "IMPORT …", "PROJECT X samples=…",
     "M2 PATTERN …" lines are the ones to send back.
"""
import argparse
import collections
import json
import pathlib
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sp16prj as P  # noqa: E402


def short(v):
    if v.kind == "binary":
        return f"<binary {len(v.value)} B>"
    if v.kind == "array":
        inner = ", ".join(short(e) for e in v.value[:8])
        return f"[{inner}{', …' if len(v.value) > 8 else ''}] ({len(v.value)})"
    if v.kind in ("void", "undefined"):
        return v.kind
    return f"{v.value!r} ({v.kind})"


def text(n, depth=0, out=sys.stdout):
    pad = "  " * depth
    out.write(f"{pad}{n.type}\n")
    for name, v in n.props:
        out.write(f"{pad}  .{name} = {short(v)}\n")
    for c in n.children:
        text(c, depth + 1, out)


def steps(n, path=""):
    here = f"{path}/{n.type}" if path else n.type
    if n.type.startswith("PatternData"):
        blob = n.get("Steps")
        if blob is not None and blob.kind == "binary":
            try:
                cells = P.decode_steps(blob.value)
            except Exception as e:  # noqa: BLE001 - report and go on
                print(f"{here}: Steps unreadable: {e}")
                cells = {}
            for (s, t), (trig, v2, typ, words) in sorted(cells.items()):
                locks = [(k, P.unpack_word(w)) for k, w in enumerate(words)]
                odd = [(k, u) for k, u in locks if not u["empty"] or u["idx"] != P.none_index(k)]
                if trig == -127 and v2 == -126 and typ == 15 and not odd:
                    continue
                ls = " ".join(f"{k}:{'none' if u['empty'] else u['value']}"
                              f"{'/vct' + str(u['vct']) if u['vct'] else ''}{'/idx' + str(u['idx']) if u['idx'] != (k if not u['empty'] else P.none_index(k)) else ''}"
                              for k, u in odd)
                coords = ("" if trig == -127 else
                          " coords=ok" if (trig, v2) == (t, s) else f" coords=MISMATCH(track {trig + 1} step {v2 + 1})")
                print(f"{here} step {s + 1:2} track {t + 1:2}: first={trig} second={v2} type={typ}{coords} {ls}")
    for c in n.children:
        steps(c, here)


def names(n, acc):
    base = n.type.rstrip("0123456789") or n.type
    acc[base].update((name, v.kind) for name, v in n.props)
    for c in n.children:
        names(c, acc)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--steps", action="store_true")
    ap.add_argument("--roundtrip", action="store_true")
    ap.add_argument("--names", action="store_true")
    a = ap.parse_args()
    data = pathlib.Path(a.file).read_bytes()
    root, compressed = P.read_file(data)
    if a.roundtrip:
        again = P.container(root) if compressed else P.enc_node(root)
        tree_same = (P.enc_node(root) == (zlib.decompress(data[8:]) if compressed else data))
        if again == data:
            print(f"roundtrip: identical ({len(data)} bytes)")
        else:
            at = next((i for i, (x, y) in enumerate(zip(again, data)) if x != y), min(len(again), len(data)))
            print(f"roundtrip: DIFFERS at byte {at} ({len(again)} vs {len(data)} bytes; tree "
                  f"{'identical, only zlib output differs' if tree_same else 'differs'})")
        return 0 if again == data else 1
    if a.names:
        acc = collections.defaultdict(set)
        names(root, acc)
        for base in sorted(acc):
            print(base + ": " + ", ".join(f"{n}:{k}" for n, k in sorted(acc[base])))
        return 0
    if a.json:
        print(json.dumps(P.node_json(root), indent=1))
    else:
        text(root)
    if a.steps:
        steps(root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
