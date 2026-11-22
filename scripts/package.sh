#!/bin/sh
# Release packaging from out/armel/<name> (scripts/build-armel.sh):
#   out/package/<name>                                the device binary as the boot menu runs it (copy it next to the
#                                                     boot menu's autoexec.bin on the USB stick; the file name is the app id)
#   out/package/<name>-<version>-armel.tar.gz         the same binary + licences + INSTALL.txt
#   out/package/<name>-<version>-emulator-mod.tar.gz  an sp16-emulator mod folder (mod.json + files/root/pdj/EP085_Main_R)
# Contains only this project's binary and texts: no firmware.
set -eu
cd "$(dirname "$0")/.."
eval "$(python3 tools/identity.py --shell)"
bin=out/armel/$PROJECT_NAME
[ -f "$bin" ] || { echo "no $bin: run scripts/build-armel.sh first" >&2; exit 1; }
stem=$PROJECT_NAME-$PROJECT_VERSION
work=out/package/work
rm -rf "$work" && mkdir -p "$work/$stem-armel" "$work/$stem-emulator-mod/files/root/pdj"
cp "$bin" "$work/$stem-armel/$PROJECT_NAME"
cp LICENSE THIRD_PARTY.md "$work/$stem-armel/"
cat > "$work/$stem-armel/INSTALL.txt" <<TXT
$PROJECT_DISPLAY_NAME $PROJECT_VERSION for the SP-16 (armel, glibc 2.13).
Copy $PROJECT_NAME to the root of a FAT32 USB stick, next to the boot menu's autoexec.bin (the bootmenu project
of the same GitHub organisation). Keep the file name exactly $PROJECT_NAME. Plug the stick into the instrument once
its own software is running and choose $PROJECT_NAME in the boot menu. Nothing is written to the instrument's firmware; the app's data goes to
$PROJECT_NAME-data/ on the stick.
Settings: key=value lines in $PROJECT_NAME.conf on the stick (passed as ${PROJECT_ENV_PREFIX}_<KEY> variables).
TXT
cp "$bin" "$work/$stem-emulator-mod/files/root/pdj/EP085_Main_R"
printf '{"schema": 1, "id": "%s", "base_profile": "sp16-1.60", "description": "%s %s"}\n' \
  "$PROJECT_NAME" "$PROJECT_DISPLAY_NAME" "$PROJECT_VERSION" > "$work/$stem-emulator-mod/mod.json"
printf 'render=auto\ncompare=0\n' > "$work/$stem-emulator-mod/$PROJECT_NAME.conf"
cp LICENSE THIRD_PARTY.md "$work/$stem-emulator-mod/"
mkdir -p out/package
cp "$bin" "out/package/$PROJECT_NAME"
tar -C "$work" -czf "out/package/$stem-armel.tar.gz" "$stem-armel"
tar -C "$work" -czf "out/package/$stem-emulator-mod.tar.gz" "$stem-emulator-mod"
rm -rf "$work"
(cd out/package && shasum -a 256 "$PROJECT_NAME" "$stem-armel.tar.gz" "$stem-emulator-mod.tar.gz" > "$stem.sha256")
ls -l out/package
