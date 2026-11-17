#!/bin/sh
# Builds the firmware and creates dist/ with the separate bins + one merged bin (flash at 0x0)
set -e
cd "$(dirname "$0")/.."
# Use a local PlatformIO venv if present, otherwise pio/python from PATH
[ -d "$HOME/.pio-venv/bin" ] && export PATH=$HOME/.pio-venv/bin:$PATH
PY=$(command -v python3 || command -v python)
[ -x "$HOME/.pio-venv/bin/python" ] && PY=$HOME/.pio-venv/bin/python
pio run -e esp32dev
B=.pio/build/esp32dev
APP0=$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
mkdir -p dist
cp $B/bootloader.bin $B/partitions.bin $B/firmware.bin "$APP0" dist/
"$PY" -m esptool version >/dev/null 2>&1 || "$PY" -m pip install -q esptool
"$PY" -m esptool --chip esp32 merge-bin -o dist/matrix-klok-merged.bin \
  --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x1000 dist/bootloader.bin 0x8000 dist/partitions.bin 0xe000 dist/boot_app0.bin 0x10000 dist/firmware.bin
(cd dist && sha256sum *.bin > SHA256SUMS.txt)
ls -l dist
