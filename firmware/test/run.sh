#!/bin/sh
# Host tests for the update path (run in any container with gcc and python3):
#   sh firmware/test/run.sh            from the repository root
set -e
T=$(mktemp -d)
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -fno-pie -no-pie \
    -DPTBOOT_TEST -DOLD_APP_SIZE=0x18000 -Wl,--defsym,__flash_binary_end=0x10020000 \
    -Istubs -pthread -o "$T/sim" sim.c ../ptboot/ptboot.c ../picotunewh/ota.c
echo "== selftest"
"$T/sim" selftest

echo "== end to end: ptupdate.py -> ota.c -> ptboot"
python3 - "$T" <<'PY'
import struct, sys, random
t = sys.argv[1]
def image(size, seed):
    random.seed(seed)
    b = bytearray(random.getrandbits(8) for _ in range(size))
    b[0x100:0x108] = struct.pack('<II', 0x20042000, 0x10009001)
    return bytes(b)
open(f'{t}/old.bin', 'wb').write(image(0x18000, 11))
open(f'{t}/new.bin', 'wb').write(image(126000, 12))
PY
"$T/sim" mkflash "$T/flash.img" "$T/old.bin"
"$T/sim" serve "$T/flash.img" 19920 > "$T/serve.log" &
sleep 0.5
python3 ../../tools/ptupdate.py 127.0.0.1 --port 19920 --info
if python3 ../../tools/ptupdate.py 127.0.0.1 --port 19920 --key wrong "$T/new.bin" 2>"$T/err"; then exit 1; fi
grep -q "error key" "$T/err" && echo "wrong key refused (ok)"
python3 ../../tools/ptupdate.py 127.0.0.1 --port 19920 --no-wait "$T/new.bin"
wait
cat "$T/serve.log"
NEW=$(python3 -c "import zlib,sys;print('%08x'%zlib.crc32(open(sys.argv[1],'rb').read()))" "$T/new.bin")
OLD=$(python3 -c "import zlib,sys;print('%08x'%zlib.crc32(open(sys.argv[1],'rb').read()))" "$T/old.bin")
echo "new image $NEW, old image $OLD"
for n in 1 2 3; do "$T/sim" boot "$T/flash.img"; done
cp "$T/flash.img" "$T/flash-trial.img"
"$T/sim" boot "$T/flash.img" | tee "$T/b4"
grep -q "app $OLD" "$T/b4" && echo "4th boot without confirmation: old image back (ok)"
"$T/sim" confirm "$T/flash-trial.img"
for n in 1 2 3 4 5; do "$T/sim" boot "$T/flash-trial.img"; done | tail -1 | tee "$T/b5"
grep -q "app $NEW" "$T/b5" && echo "confirmed: new image stays (ok)"
rm -rf "$T"
