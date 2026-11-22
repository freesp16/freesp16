#!/bin/sh
# Fail if any tracked file other than cmake/ProjectIdentity.cmake (and README.md / CONTRIBUTING.md, which link to the
# project's repository and website) contains a copy of the project's name, display name or identifier (case-insensitive
# for the name), and check the display name for third-party trademarks the spec forbids in the product's UI and
# distribution (07-ui-design, 00-overview/sources-and-licensing.md).
# Usage: tools/check-identity.sh   (from anywhere inside the repository)
set -eu
cd "$(dirname "$0")/.."
eval "$(python3 tools/identity.py --shell)"

status=0
files=$(git ls-files -co --exclude-standard | grep -v -E '^(cmake/ProjectIdentity\.cmake|README\.md|CONTRIBUTING\.md|third_party/)' || true)

check() { # label, grep flags, pattern
  hits=$(printf '%s\n' "$files" | while IFS= read -r f; do [ -f "$f" ] && grep -I -l $2 -e "$3" "$f" 2>/dev/null || true; done)
  if [ -n "$hits" ]; then
    echo "identity: hard-coded $1 '$3' in:"; printf '  %s\n' $hits; status=1
  fi
}
check "project name" "-i -F" "$PROJECT_NAME"
check "display name" "-F" "$PROJECT_DISPLAY_NAME"
check "identifier" "-F" "$PROJECT_ID"

# The display name and package names reach users: no third-party trademarks in them.
for mark in "SP-16" "SP16" "Pioneer" "TORAIZ" "Toraiz"; do
  case "$PROJECT_DISPLAY_NAME" in *"$mark"*) echo "identity: display name contains the trademark '$mark'"; status=1;; esac
done

[ $status -eq 0 ] && echo "identity: ok (name, display name and id appear only in cmake/ProjectIdentity.cmake)"
exit $status
