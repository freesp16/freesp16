#!/bin/sh
# Rename the project. Everything derives from cmake/ProjectIdentity.cmake, so this rewrites that one file, then
# checks that no stale copy of the old name is left anywhere.
# Usage: tools/rename.sh <name> "<Display Name>" <reverse.dns.id> [version]
#   tools/rename.sh mysampler "My Sampler" org.example.mysampler
# Afterwards: delete build/ (the CMake cache holds the old target names) and rebuild.
set -eu
cd "$(dirname "$0")/.."
[ $# -ge 3 ] || { sed -n '2,6p' "$0" >&2; exit 2; }
name=$1 display=$2 id=$3
version=${4:-$(python3 tools/identity.py VERSION)}
printf '%s' "$name" | grep -Eq '^[a-z][a-z0-9-]*$' || { echo "name: lowercase letters, digits, hyphens" >&2; exit 2; }
printf '%s' "$id" | grep -Eq '^[a-z][a-z0-9-]*(\.[a-z0-9-]+)+$' || { echo "id: reverse DNS, e.g. org.example.app" >&2; exit 2; }
case "$display" in *'"'*) echo "display name must not contain double quotes" >&2; exit 2;; esac

python3 - "$name" "$display" "$id" "$version" <<'EOF'
import pathlib, re, sys
name, display, ident, version = sys.argv[1:]
path = pathlib.Path("cmake/ProjectIdentity.cmake")
text = path.read_text(encoding="utf-8")
for key, value in (("NAME", name), ("DISPLAY_NAME", display), ("ID", ident), ("VERSION", version)):
    text, n = re.subn(rf'^set\(PROJECT_IDENTITY_{key}\s+"[^"]*"\)$', f'set(PROJECT_IDENTITY_{key} "{value}")', text, flags=re.M)
    if n != 1:
        sys.exit(f"could not rewrite PROJECT_IDENTITY_{key}")
path.write_text(text, encoding="utf-8")
EOF
echo "identity is now:"; python3 tools/identity.py | sed 's/^/  /'
tools/check-identity.sh
echo "next: rm -rf build out && cmake --preset host && cmake --build --preset host"
