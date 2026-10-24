#!/bin/sh
# Fetches the WIZnet RP2040-HAT-C SDK the firmware is built with, at the exact commit used for the
# releases (2024-08-08, pico-sdk 1.4.0, ioLibrary W5500_PHY_LINK-86), into build/RP2040-HAT-C:
#   sh tools/build/fetch-sdk.sh            (from the repository root)
# tools/build/build.sh then patches and builds it (see docs/firmware.md).
set -e
HATC_SHA=076f4cedbe1f12ea7d4e565ec1f31e1bfc27fe2b
DIR=${1:-build/RP2040-HAT-C}
if [ ! -d "$DIR/.git" ]; then
    git clone https://github.com/Wiznet/RP2040-HAT-C.git "$DIR"
fi
cd "$DIR"
git fetch --quiet origin
git checkout --quiet "$HATC_SHA"
git submodule update --init --recursive
# the submodule commits come from HATC_SHA; print them for the build log
git rev-parse HEAD
git submodule status --recursive | grep -E 'ioLibrary_Driver|libraries/pico-sdk \(' || true
