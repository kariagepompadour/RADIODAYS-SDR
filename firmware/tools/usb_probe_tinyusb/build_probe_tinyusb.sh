#!/bin/bash
# Builds the TinyUSB CDC probe (USB-OTG, CDC on boot) for comparison with the Hardware CDC probe in ../usb_probe and
# writes firmware/bin/USB-PROBE-TINYUSB.ino.bin, an app-only image that the Launcher can start without touching itself.
#   ./build_probe_tinyusb.sh
set -e
cd "$(dirname "$0")"
REPO=$(cd ../../.. && pwd)
MAPS="-ffile-prefix-map=$REPO=/repo -ffile-prefix-map=$HOME=/home/user"
OUT=$(mktemp -d)
arduino-cli compile --clean --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=default_8MB,FlashSize=8M,USBMode=default,CDCOnBoot=cdc \
  --build-property "compiler.c.extra_flags=$MAPS" --build-property "compiler.cpp.extra_flags=$MAPS" \
  --output-dir "$OUT" .
mkdir -p ../../bin
cp "$OUT"/usb_probe_tinyusb.ino.bin ../../bin/USB-PROBE-TINYUSB.ino.bin
shasum -a 256 ../../bin/USB-PROBE-TINYUSB.ino.bin
rm -rf "$OUT"
