#!/bin/bash
# Builds the USB probe with the radio firmware's FQBN and USB settings (Hardware CDC, CDC on boot) and writes
# firmware/bin/USB-PROBE-HWCDC.ino.bin, an app-only image that the Launcher can start without touching itself.
#   ./build_probe.sh
set -e
cd "$(dirname "$0")"
REPO=$(cd ../../.. && pwd)
MAPS="-ffile-prefix-map=$REPO=/repo -ffile-prefix-map=$HOME=/home/user"
OUT=$(mktemp -d)
arduino-cli compile --clean --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=default_8MB,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc \
  --build-property "compiler.c.extra_flags=$MAPS" --build-property "compiler.cpp.extra_flags=$MAPS" \
  --output-dir "$OUT" .
mkdir -p ../../bin
cp "$OUT"/usb_probe.ino.bin ../../bin/USB-PROBE-HWCDC.ino.bin
shasum -a 256 ../../bin/USB-PROBE-HWCDC.ino.bin
rm -rf "$OUT"
