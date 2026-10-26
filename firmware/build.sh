#!/bin/bash
# Builds the firmware for the M5Stack Cardputer ADV with arduino-cli and writes firmware/bin/ + SHA256SUMS.txt.
# Needs (see README): arduino-cli, the "m5stack:esp32" core 3.3.10, libraries M5Cardputer, M5Unified, M5GFX, ArduinoJson 7.
#   ./build.sh
# The compiler is told to replace the build machine's folders ($HOME and this repository) in the paths it embeds,
# so the published .bin files do not carry your user name or folder layout.
# If arduino-cli's data/user folders are outside $HOME, list them: EXTRA_PREFIX_MAPS="/path/to/arduino_data=/arduino" ./build.sh
set -e
cd "$(dirname "$0")"
REPO=$(cd .. && pwd)
MAPS="-ffile-prefix-map=$REPO=/repo -ffile-prefix-map=$HOME=/home/user"
for m in $EXTRA_PREFIX_MAPS; do MAPS="$MAPS -ffile-prefix-map=$m"; done
VERSION=$(sed -n 's/^#define BUILD_TAG "\(.*\)"$/\1/p' RadioDays_SDR/RadioDays_SDR.ino)
[ -n "$VERSION" ] || { echo "BUILD_TAG not found in RadioDays_SDR.ino" >&2; exit 1; }
OUT=$(mktemp -d)
arduino-cli compile --clean --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=default_8MB,FlashSize=8M,USBMode=default,CDCOnBoot=cdc \
  --build-property "compiler.c.extra_flags=$MAPS" --build-property "compiler.cpp.extra_flags=$MAPS" \
  --output-dir "$OUT" RadioDays_SDR
mkdir -p bin
cp "$OUT"/RadioDays_SDR.ino.bin "$OUT"/RadioDays_SDR.ino.bootloader.bin "$OUT"/RadioDays_SDR.ino.partitions.bin "$OUT"/RadioDays_SDR.ino.merged.bin bin/
# Versioned copies (named after BUILD_TAG) for Launcher / M5Burner uploads. They stay local (.gitignore), so the
# checksum list covers only the four release images that the repository publishes.
cp "$OUT"/RadioDays_SDR.ino.bin "bin/$VERSION.ino.bin"
cp "$OUT"/RadioDays_SDR.ino.merged.bin "bin/RDSDR_${VERSION}_MERGED.bin"
(cd bin && shasum -a 256 RadioDays_SDR.ino.bin RadioDays_SDR.ino.bootloader.bin RadioDays_SDR.ino.merged.bin \
  RadioDays_SDR.ino.partitions.bin > SHA256SUMS.txt && cat SHA256SUMS.txt)
rm -rf "$OUT"
