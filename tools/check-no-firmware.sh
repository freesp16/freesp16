#!/bin/sh
# Fail if the repository contains anything that looks like vendor firmware, keys, update files, data images or
# session captures. Checks tracked and untracked-but-not-ignored files, excluding third_party/.
set -eu
cd "$(dirname "$0")/.."
status=0
files=$(git ls-files -co --exclude-standard | grep -v '^third_party/' || true)
for f in $files; do
  [ -f "$f" ] || continue
  case "$f" in
    *.UPD|*.upd|*.key|*.sqfs|*.squashfs|*.img|*.prj|*.bin|*.elf)
      echo "no-firmware: forbidden file type: $f"; status=1; continue;;
  esac
  case "$f" in */sessions/*|sessions/*|private/*|*/private/*)
      echo "no-firmware: session/private data: $f"; status=1; continue;; esac
  magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
  case "$magic" in
    68737173|73717368) echo "no-firmware: squashfs image: $f"; status=1;;     # hsqs / sqsh
    7f454c46) echo "no-firmware: ELF binary: $f"; status=1;;
  esac
  size=$(wc -c < "$f")
  case "$f" in resources/fonts/*) ;; *)
    if [ "$size" -gt 2097152 ]; then echo "no-firmware: file over 2 MB (review it): $f"; status=1; fi;;
  esac
done
# Fixtures must be generated, never captured: no 64-hex-digit (AES-256-like) blobs in tests/ or resources/.
# Checksums are allowed: lines that say sha256, `sha256sum` manifest lines ("<hex>  <file>") and pinned download
# checksums ("<archive>.zip|.ttf|.otf": "<hex>"). (\b works in GNU git grep -E; BSD regex matched nothing here.)
if git grep -n -I -E '(^|[^0-9a-fA-F])[0-9a-fA-F]{64}([^0-9a-fA-F]|$)' -- tests resources 2>/dev/null \
    | grep -v -E 'sha256|SHA-256' \
    | grep -v -E '^[^:]+:[0-9]+:[0-9a-f]{64}  [^ ]+$' \
    | grep -v -E '^[^:]+:[0-9]+: *"[^"]+\.(zip|ttf|otf)": "[0-9a-f]{64}",?$' ; then
  echo "no-firmware: key-like hex blob in tests/ or resources/ (see above)"; status=1
fi
[ $status -eq 0 ] && echo "no-firmware: ok"
exit $status
