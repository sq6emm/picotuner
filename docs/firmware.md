# PicoTuneWH 0v3e-ota3 (own build)

G4EWJ's 0v3e source (GPL v3, BATC wiki `Ptwh0v3e-source.zip`) in `firmware/picotunewh/`, plus:

1. **DHCP:** the REQUEST carries option 54 (server identifier) from the OFFER, falling back to
   the OFFER's source address; option 12 (host name) stays. Fixes the MikroTik problem the way
   0v3g does, but also behind a relay. Renewals (state 7 at T/2) send the same packet.
2. **Signal levels in the status** (UDP 9901/9902…, every 500 ms, locked or not):
   `$35` AGC1 (STV0910 `AGCIQIN1:0`, LongMynd `$26`), `$36` AGC2 (`AGC2I1:0`, LongMynd `$27`),
   `$4`/`$5` POWER I/Q, `$2` LNA gain `(gain<<5)|vgo` (NIMs with the STVVGLNA only).
   26/27 are G4EWJ's multistream numbers, hence 35/36.
   Also from the demodulator, in G4EWJ's/LongMynd's numbering (defined in `main.h`, never filled
   before): `$7`/`$8` one constellation symbol I/Q (signed), `$28` TSSTATUS2:TSSTATUS (TS FIFO);
   when locked `$11` BER; DVB-S only `$3` puncture (n of FEC n/(n+1)), `$10` Viterbi error rate
   (% x 100); DVB-S2 only `$21` LDPC and `$22` BCH error counts, `$23` uncorrected flag (these
   three are chip totals, not per demodulator), `$26`/`$27` multistream (MATSTR/PDELCTRL,
   ISIENTRY/ISIBITENA). Fields that do not apply to the current state are left empty.
3. **Update over Ethernet** with the `ptboot` bootloader (`firmware/ptboot/`, layout in
   `firmware/common/ptupdate.h`), on the command port, no extra socket.
4. Version `ptwh0v3e-ota2-w5100s.uf2`.

5. **Band scan inside the firmware (0v3e-ota3)**: a tune command with `scanf=` makes Core1 step
   through the channels itself, checking the demodulator every 40 ms:
   `[to@wh] rcv=2 offset=9750000 fplug=A vgx=hi scanf=10492750-10499250/500 scansr=500,333`
   - `scanf=` `start-stop/step` or `f1,f2,...` kHz (max 48), `scansr=` up to 4 kS/s values,
     `scanms=` optional wait for a lock (default 400 ms + 600 s / SR, x3 once DVB-S2 headers show).
   - A lock skips the frequency's other symbol rates and is reported 2.5 s later to UDP 9905 of
     the commanding host: `[from@wh:SCAN] rx= freq= srate= state= mer= modcod= lock=ms name=
     provider= agc1=`; each pass ends with `[from@wh:SCANPASS] rx= pass= found= time=ms`.
   - Runs until the next tune command for that receiver (`freq=0` switches it off). Off-net hosts
     must repeat the command within 15 min, as for any tune. `tools/qo100scan.py` does all that.
   - Also fixed: commands longer than 255 characters overflowed `commandrxbuff`.
   Untested on hardware so far: parser checked on the host, builds without warnings.

## Build
On `power`, in Docker (Ubuntu 20.04's apt GCC 9.2, like G4EWJ's `apt install gcc-arm-none-eabi`):
```
rsync -az firmware tools user@build-host:~/work/picotuner/
ssh user@build-host 'docker run --rm -u $(id -u):$(id -g) -e PTUP_KEY=... \
    -v ~/work/picotuner:/w picotuner-build:focal sh /w/tools/build/build.sh'
```
Needs `~/work/picotuner/build/RP2040-HAT-C` (2024-08-08, pico-sdk 1.4.0) and the image from
`tools/build/Dockerfile.focal`. Output in `~/work/picotuner/out/`:
`picotunewh-full.uf2` (ptboot + app, first install over USB), `picotunewh.bin` (updates).
Without `PTUP_KEY` the key is `picotuner`.

Tests (host, no hardware): `sh firmware/test/run.sh` — ota.c and ptboot.c against a NOR-flash
model at 0x10000000: install, rollback, a power cut at every flash operation (119 install /
49 rollback), corrupt staging, and ptupdate.py → ota.c → ptboot over real UDP.

## Install, update, way back
- **First time (USB):** `[to@wh] bootsel=369` to port 9920 (or the BOOTSEL button), then copy
  `picotunewh-full.uf2` to the RPI-RP2 drive.
- **Updates:** `python3 tools/ptupdate.py TUNER_IP --key KEY picotunewh.bin`
  (`--info` shows the state). ~125 KB, stop-and-wait 1 KiB packets. The tuner reboots; ptboot
  backs up the running app, installs the new one, and the new one confirms itself after 60 s
  with an IP address. Three boots without that → ptboot puts the old one back.
- **Back to G4EWJ's firmware:** BOOTSEL + copy `fw/orig/xwh0v3g-w5100s.uf2` (it overwrites
  ptboot; the update metadata left at 0x101f7000 is ignored).

## Tested on a bare W5100S-EVB-Pico (no NIM), 2026-10-22
- ptboot → app hand-over, USB CDC log, DHCP (TEST_BOARD_IP from ROUTER_IP), runs with
  "NIM_A could not be initialised" and the built-in QO-100 ini commands.
- Upload with `ptupdate.py`: 125 KB at 64–67 KiB/s (~2 s), wrong key refused, reboot into trial,
  confirmed after 60 s. Rollback: an image rebooted (`reboot=258`) on each of its 3 trial boots
  was replaced by the previous one on the 4th; the broadcast shows the old version again.
- Installing through the ROM's USB drive: a UF2 with a gap between ptboot and the app came out
  with only the last 1 KiB of every app sector written (once), and one block missing (the next
  time). `uf2pack.py` now fills the gap; that image read back identical. Check with
  `picotool save` + compare, or install with `picotool load -v`.
- Same set on a bare W5500-EVB-Pico (`WIZNET_CHIP=W5500` build, ptwh0v3e-ota2-w5500): DHCP,
  ota1 → rbtest → ota1 with confirmation, rollback after 3 trial boots, `bootsel=369` over the
  network. Its WIZnet loopback demo is saved in `fw/testboard/w5500/w5500-before.bin`.
- Upload speed: 82–84 KiB/s from `power` (wired), 60–75 KiB/s from the notebook on Wi-Fi with
  two slow runs (2 and 7 KiB/s, few resends; never seen on the wired host). Outside a session the
  command port is polled every 250 ms as in the original, so a lone INFO takes ~250 ms.
- Not covered without a NIM: the `$2/$3/$4/$5/$7/$8/$10/$11/$21-23/$26-28/$35/$36` values, I2C under the Core1 lockout, TS.

## Cold start: crystal start-up (found on the test W5100S-EVB-Pico)
After a power-on the test board stayed dark (no USB, no LED) with every SDK 1.4.0 build at the
default `PICO_XOSC_STARTUP_DELAY_MULTIPLIER=1`: ptboot + app, the app without ptboot, a 2.5 s
delay in ptboot, and G4EWJ's own `ptwh0v3e-w5100s.uf2`. Warm resets (picotool, `reboot=258`,
updates) always worked. With the multiplier at 64 (about 64 ms instead of 1 ms of XOSC start-up)
it started 3 times out of 3. Both CMake projects now set it; it costs ~60 ms at boot.
Bring-up options in `tools/build/build.sh`: `PTBOOT_DEBUG_LED=1` (ptboot blinks GP25: 1 long =
running, 2 short = starting the app, 5 fast = no app, BOOTSEL), `PTBOOT_START_DELAY_MS=n`,
`STANDALONE=1` (app at 0x10000000 without ptboot), `XOSC_MULT=n`, `VERSION_TAG=x`.

## Limits / risks
- Still to try on the real tuner: AGC/power/LNA values, Core1 lockout while it waits on I2C,
  TS during sector writes. ptboot itself is only replaced over USB.
- Each 4 KiB sector write pauses Core1 and interrupts on Core0 for tens of ms: TS drops packets
  during an upload (the status/command path keeps working).
- The key travels in clear text on the LAN; it stops accidents, not an attacker on the network.
- An image that runs but breaks networking is only caught by the 3-boot rule, so it needs power
  cycles (or USB BOOTSEL, which always works: it is in the RP2040's ROM).
