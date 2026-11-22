#!/bin/sh
# Install the device binary into an emulator mod folder as /root/pdj/EP085_Main_R, using
# sp16-custom-app/scripts/install-mod.sh (copy + `./sp16 mod check` preflight), plus the app's settings file.
#   scripts/install.sh [settings...]      e.g. scripts/install.sh render=soft compare=1 compare_frames=300
# Environment: SP16_EMULATOR (default ~/src/sp16-emulator), SP16_CUSTOM_APP (default ~/src/sp16-custom-app),
#              APP_MOD (default $SP16_EMULATOR/private/<name>). The mod lives under the emulator's git-ignored private/.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
eval "$(python3 "$here/tools/identity.py" --shell)"
emu=${SP16_EMULATOR:-$HOME/src/sp16-emulator}
custom=${SP16_CUSTOM_APP:-$HOME/src/sp16-custom-app}
mod=${APP_MOD:-$emu/private/$PROJECT_NAME}
bin=$here/out/armel/$PROJECT_NAME
[ -f "$bin" ] || { echo "no $bin: run scripts/build-armel.sh first" >&2; exit 1; }
[ -x "$emu/sp16" ] || { echo "no emulator at $emu (set SP16_EMULATOR)" >&2; exit 1; }
[ -f "$custom/scripts/install-mod.sh" ] || { echo "no $custom/scripts/install-mod.sh (set SP16_CUSTOM_APP)" >&2; exit 1; }
mkdir -p -m 700 "$emu/private"
# Create the mod with this project's id first (install-mod.sh would otherwise create it as "my-app").
[ -f "$mod/mod.json" ] || (cd "$emu" && ./sp16 mod new --mod "$mod" --id "$PROJECT_NAME" >/dev/null)
SP16_EMULATOR=$emu sh "$custom/scripts/install-mod.sh" "$bin" "$mod" >/dev/null
# Settings file, read by the app from the dev overlay share (/opt/sp16/dev-share/<name>.conf).
: > "$mod/$PROJECT_NAME.conf"
for kv in "$@"; do echo "$kv" >> "$mod/$PROJECT_NAME.conf"; done
echo "installed $bin -> $mod/files/root/pdj/EP085_Main_R (settings: ${*:-defaults})"
