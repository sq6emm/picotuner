# PicoTuneWH 0v3e-ota17 (own build)

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
     `scanms=` optional wait for a lock (default 400 ms + 600 s / SR in ota3, 400 ms + 400 s / SR since
     ota13; x3 once DVB-S2 headers show).
   - A lock skips the frequency's other symbol rates and is reported 2.5 s later to UDP 9905 of
     the commanding host: `[from@wh:SCAN] rx= freq= srate= state= mer= modcod= lock=ms name=
     provider= agc1=`; each pass ends with `[from@wh:SCANPASS] rx= pass= found= time=ms`.
   - Runs until the next tune command for that receiver (`freq=0` switches it off). Off-net hosts
     must repeat the command within 15 min, as for any tune. `tools/qo100scan.py` does all that.
   - Also fixed: commands longer than 255 characters overflowed `commandrxbuff`.
   Untested on hardware so far: parser checked on the host, builds without warnings.

6. **Update robustness (0v3e-ota4 .. ota11)**: Core1 parks itself in a RAM loop (not inlined:
   `__no_inline_not_in_flash_func`) while Core0 erases/programs, every wait bounded; TS capture off
   for an update session; no printf in the update path; 8 s watchdog fed only while both cores run
   (Core1 heartbeat); trace points in watchdog scratch 0-3, shown by `ptupdate.py --info` after a
   watchdog reset; GP23 high (3V3 regulator in PWM mode). `$11` BER only for DVB-S, `$23` fixed.
   Details: `docs/agc-test-results-2026-10-23.md`.

7. **More demodulator data (0v3e-ota12)**: `$37` raw noise estimate (NNOSPLHT in DVB-S2, NNOSDATAT
   in DVB-S; falls as C/N rises, about 3850 at MER 11.3 dB), `$38` lock flags (bit 0 carrier,
   bits 1-2 timing quality, bit 3 demodulator, bit 4 FEC, bit 5 TS, bit 6 ADC overload; 63 = all
   locked), `$39` bad TS packets since the lock (error counter 2, reset at each new lock), `$40`
   15 constellation symbols as hex `IIQQ` pairs (signed bytes) per status cycle.
   `AGCMS=n` (20..1000) in a tune command streams `[from@wh:AGC] rx= t= agc1= agc2= pi= pq= state=`
   every n ms to port 9906 (base + 6) for antenna scans; the next command without it stops it.
   The scanner reports a station only if it is still locked after the hold time. Deliberate
   reboots (command, update) are marked, so `--info` reports only real watchdog resets.
   C/N in dB is not computed in the firmware: the known tables are GPL-2.0-only (dddvb).

8. **Fast band scan (0v3e-ota13)**: each pass first screens every frequency/symbol rate by AGC2
   (25 ms each; a carrier lowers AGC2 against a local noise reference, the highest AGC2 within
   1.6 MHz), then tries a lock only on the candidates; the lock timeout is 400 ms + 400 s / SR
   (kS/s); a lock is held 300 ms, every 10th pass 2.5 s for the service name (other passes report
   the last name seen). Carriers that do not lock are retried only every 8th pass (16th since ota16), staggered.
   Groups: `SCANF=a;b;c SCANSR=x;y;z` give each frequency list its own symbol rates
   (QO-100 WB bandplan: 500 kS/s only up to 10496.75 MHz). `SCANSCREEN=ms` changes the screen
   wait. The pass line carries `screen=`, `candidates=` and all AGC2 readings. On 2026-10-24,
   with `tools/qo100scan.py --rx 1,2` (plan split over both receivers) and an empty band, a pass
   took 2.1-2.3 s (median); before, 54 s for the narrow channels alone.

9. **Fixed gain (0v3e-ota14)**: `AGCHOLD=1` in a tune command holds the STVVGLNA gain (RFAGC mode
   "auto request"; the LNA is shared by both receivers on that input) and AGC1 (`AGCIQ_BETA` 0)
   1.5 s after the tune. Without it the front end normalises the total LNB power, so AGC2 is only
   the channel's share of it (pointing at the geostationary arc, with strong TV satellites next to
   QO-100, lowers the LNA gain). Held, AGC2 and the power registers follow the antenna power. The
   next tune command without `AGCHOLD=1` releases it; `$38` bit 7 = held; the `AGCMS=` stream adds
   `vgo=` (the LNA detector reading, which keeps changing while held) and `hold=`.
   Beacon at the peak / 5 deg above the arc: AGC2 149 / 366 automatic, 150 / 465 held.

10. **Codecs in scan reports (0v3e-ota15)**: `[from@wh:SCAN]` lines end with `video=` (H262, H264,
    H265, H266) and `audio=` (MPA, AAC, AC3) from the station's TS, empty until the TS has shown
    them (so mostly on the passes that hold for the service name); cleared at every scan retune.

11. **Fewer wasted lock attempts (0v3e-ota16)**: a candidate is skipped when its occupied band
    overlaps a signal already found (|df| < (SR1 + SR2) x 0.675 kHz, i.e. SR x 1.35 / 2 each),
    a receiver locked outside a scan (someone watching it) counts as found, and carriers that do
    not lock are retried every 16th pass. With RX2 scanning the whole plan alone while RX1 shows
    the beacon, a pass went from 5.5-25.8 s to 2.7-8 s.

12. **Robustness fixes from a code review (0v3e-ota17)**:
    - a command longer than about 235 bytes overflowed `lastincomingcommand` (inherited); now
      cut at 200 characters;
    - I2C transfers have a 10 ms timeout instead of waiting for ever on a stuck bus (Core1 used
      to stop until the watchdog, again and again); failures are counted in `$41`;
    - PTUP replies are always terminated (a long state text sent stack bytes);
    - BEGIN is refused while the running image is on trial (it erased the metadata, losing the
      confirmation and the way back); `ptupdate.py` shows the reason and stops when an update
      makes no progress;
    - DHCP option lengths are checked and the ACK's transaction id compared;
    - scan lists are checked against the LO range and the SR limits, `SCANMS=`/`SCANSCREEN=`
      kept sane (a negative value stopped the scan);
    - the receiver switch-off on idle no longer calls I2C from Core0 (unused, idletime 0);
    - the name-collecting pass (every 10th) only waits for stations without a name yet, and
      names survive the same scan command sent again.

## Build
On `power`, in Docker (Ubuntu 20.04's apt GCC 9.2, like G4EWJ's `apt install gcc-arm-none-eabi`):
```
rsync -az firmware tools user@<build host>:~/work/picotuner/
ssh user@<build host> 'docker run --rm -u $(id -u):$(id -g) -e PTUP_KEY=... \
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
- **Updates:** `python3 tools/ptupdate.py <tuner> --key KEY picotunewh.bin`
  (`--info` shows the state). ~125 KB, stop-and-wait 1 KiB packets. The tuner reboots; ptboot
  backs up the running app, installs the new one, and the new one confirms itself after 60 s
  with an IP address. Three boots without that → ptboot puts the old one back.
- **Back to G4EWJ's firmware:** BOOTSEL + copy the original UF2 from the BATC wiki (it overwrites
  ptboot; the update metadata left at 0x101f7000 is ignored).

## Tested on a bare W5100S-EVB-Pico (no NIM), 2026-10-22
- ptboot → app hand-over, USB CDC log, DHCP (an address from the test router), runs with
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
- Not covered without a NIM: the `$2/$3/$4/$5/$7/$8/$10/$11/$21-23/$26-28/$35/$36` values, I2C while Core1
  is parked, TS (all checked later in the tuner).

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
- ptboot itself is only replaced over USB. (The SDK multicore lockout of ota2 was replaced in ota4 by
  Core1 parking itself in RAM; tested in the tuner.)
- Each 4 KiB sector write pauses Core1 and interrupts on Core0 for tens of ms: TS drops packets
  during an upload (the status/command path keeps working).
- The key travels in clear text on the LAN; it stops accidents, not an attacker on the network.
- An image that runs but breaks networking is only caught by the 3-boot rule, so it needs power
  cycles (or USB BOOTSEL, which always works: it is in the RP2040's ROM).
