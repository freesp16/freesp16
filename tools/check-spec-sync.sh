#!/bin/sh
# Check the pinned design files against the spec (UID-050):
#   tools/check-spec-sync.sh [SPEC_DIR]       default SPEC_DIR: ../sixteen-spec/spec next to the repository
# 1. resources/design/SPEC_VERSION's SHA-256 sums match the pinned copies (always; fails on a local edit).
# 2. When the spec is present, every pinned file equals the spec's file (fails on drift: re-copy the file, update
#    SPEC_VERSION, regenerate the theme and re-run the snapshot tests). Exit 0 = in sync, 1 = drift, 2 = usage.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
design="$here/resources/design"
spec=${1:-${SPEC_DIR:-}}
if [ -z "$spec" ]; then # next to the checkout, or next to a worktrees folder (../<name>-wt/<branch>)
  for d in "$here/../sixteen-spec/spec" "$here/../../sixteen-spec/spec"; do [ -d "$d" ] && { spec=$d; break; }; done
fi
found=
version="$design/SPEC_VERSION"
[ -f "$version" ] || { echo "missing $version" >&2; exit 2; }
sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }
status=0

want=$(sed -n 's/^sha256: //p' "$version")
got=$(sha "$design/tokens.json")
[ "$want" = "$got" ] || { echo "tokens.json: sha256 $got does not match SPEC_VERSION ($want)"; status=1; }
grep -E '^[0-9a-f]{64}  .+\.svg$' "$version" | while read -r sum name; do
  f="$design/mockups/$name"
  if [ ! -f "$f" ]; then echo "mockups/$name: listed in SPEC_VERSION but missing"; exit 1; fi
  [ "$(sha "$f")" = "$sum" ] || { echo "mockups/$name: sha256 differs from SPEC_VERSION"; exit 1; }
done || status=1
for f in "$design"/mockups/*.svg; do
  grep -q "  $(basename "$f")\$" "$version" || { echo "mockups/$(basename "$f"): not listed in SPEC_VERSION"; status=1; }
done

if [ -n "$spec" ] && [ -d "$spec/07-ui-design" ]; then
  found=" and $spec"
  cmp -s "$design/tokens.json" "$spec/07-ui-design/tokens.json" || { echo "tokens.json differs from $spec/07-ui-design/tokens.json"; status=1; }
  for f in "$design"/mockups/*.svg; do
    s="$spec/07-ui-design/mockups/$(basename "$f")"
    if [ ! -f "$s" ]; then echo "mockups/$(basename "$f"): no longer in the spec"; status=1
    elif ! cmp -s "$f" "$s"; then echo "mockups/$(basename "$f") differs from the spec"; status=1; fi
  done
else
  echo "(spec not found at $spec: checked the pinned sums only)"
fi
[ $status -eq 0 ] && echo "check-spec-sync: resources/design matches SPEC_VERSION$found"
exit $status
