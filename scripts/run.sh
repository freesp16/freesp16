#!/bin/sh
# Launch the emulator's research build (file audio) with the app's mod, or restart only the app if a session runs.
# Prints the emulator app's PID; stop it with `kill <pid>` (never by bundle id, never pkill).
#   scripts/run.sh [--bench]       --bench: unattended (window ignores input, opens in the background)
# Environment: SP16_EMULATOR, APP_MOD (as install.sh), SP16_BUILD (default sp16-dev: --research --audio file).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
eval "$(python3 "$here/tools/identity.py" --shell)"
emu=${SP16_EMULATOR:-$HOME/src/sp16-emulator}
mod=${APP_MOD:-$emu/private/$PROJECT_NAME}
build=${SP16_BUILD:-sp16-dev}
extra=""; [ "${1:-}" = --bench ] && extra="--no-ui-input"
cd "$emu"
if ./sp16 ctl --build "$build" metrics >/dev/null 2>&1; then
  ./sp16 ctl --build "$build" app-restart >/dev/null && echo "restarted the app in the running session"
else
  ./sp16 run --name "$build" --dev-overlay "$mod" $extra >/dev/null
  sleep 2
fi
session=$(ls -td "build/$build/sessions"/native-* | head -1)
pid=$(pgrep -f "build/$build/.*MacOS/SP16Emulator .*--dev-overlay $mod" | head -1 || true)
echo "session: $emu/$session"
echo "emulator pid: ${pid:-unknown}"
