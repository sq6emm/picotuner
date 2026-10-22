#!/bin/sh
# Builds ptboot + PicoTuneWH inside picotuner-build:focal (tools/build/Dockerfile.focal).
# Run with the project mounted at /w and RP2040-HAT-C cloned in /w/build/RP2040-HAT-C:
#   docker run --rm -v ~/work/picotuner:/w picotuner-build:focal sh /w/tools/build/build.sh
# Optional: PTUP_KEY=... for the update key, WIZNET_CHIP=W5500 for the W5500-EVB-Pico.
# VERSION_TAG=x: version 0v3e-x instead of 0v3e-ota1 (to tell test images apart).
# Bring-up: PTBOOT_DEBUG_LED=1 (ptboot blinks GP25), STANDALONE=1 (app linked at 0x10000000,
# no ptboot). Output: /w/out/ (W5100S) or /w/out/w5500/, plus -debug / -standalone suffixes.
set -e
HATC=/w/build/RP2040-HAT-C
cd "$HATC"
python3 /w/tools/build/hatc_patch.py
rm -rf examples/picotunewh examples/ptboot examples/common
cp -r /w/firmware/picotunewh /w/firmware/ptboot /w/firmware/common examples/
[ -z "$VERSION_TAG" ] || sed -i "s/#define VERSIONX2\t\"e-ota1\"/#define VERSIONX2\t\"e-$VERSION_TAG\"/" examples/picotunewh/picotunewh.c
[ -z "$STANDALONE" ] || sed -i 's/ORIGIN = 0x10008000, LENGTH = 480k/ORIGIN = 0x10000000, LENGTH = 2016k/' examples/picotunewh/memmap_custom-2MB.ld
CHIP=${WIZNET_CHIP:-W5100S}
OUT=/w/out; [ "$CHIP" = W5100S ] || OUT=/w/out/$(echo "$CHIP" | tr A-Z a-z)
SUFFIX=${PTBOOT_DEBUG_LED:+-debug}${STANDALONE:+-standalone}${VERSION_TAG:+-$VERSION_TAG}
OUT=$OUT$SUFFIX
mkdir -p "$OUT" build-ota-$CHIP$SUFFIX && cd build-ota-$CHIP$SUFFIX
cmake -DCMAKE_BUILD_TYPE=Release -DWIZNET_CHIP=$CHIP -DPTBOOT_DEBUG_LED=${PTBOOT_DEBUG_LED:-0} ${PTUP_KEY:+-DPTUP_KEY="$PTUP_KEY"} .. > $OUT/cmake.log 2>&1
make -j16 ptboot picotunewh > $OUT/make.log 2>&1 || { grep -E 'error|warning: implicit' $OUT/make.log | head -30; exit 1; }
cp examples/ptboot/ptboot.bin examples/ptboot/ptboot.elf.map $OUT/
cp examples/picotunewh/picotunewh.bin examples/picotunewh/picotunewh.uf2 examples/picotunewh/picotunewh.elf examples/picotunewh/picotunewh.elf.map $OUT/
python3 /w/tools/uf2pack.py $OUT/picotunewh-full.uf2 $OUT/ptboot.bin@0x10000000 $OUT/picotunewh.bin@0x10008000
arm-none-eabi-size examples/ptboot/ptboot.elf examples/picotunewh/picotunewh.elf
grep -c 'warning:' $OUT/make.log || true
