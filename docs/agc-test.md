# AGC test plan: 0v3e-ota2 in the real PicoTuner

Goal: check that the new status fields (`$35` AGC1, `$36` AGC2, `$4/$5` power, `$2` LNA, BER,
LDPC/BCH, constellation, TS status) give sensible values with the FTS-4334L NIM, with and without
a lock, and that reading them does not hurt reception. No dish movement.

Module: the test W5100S-EVB-Pico (ptboot + `ptwh0v3e-ota2-w5100s.uf2`, update key `picotuner`).
The original module (G4EWJ 0v3g) is the way back: swap it in again.

## Before the day
- MAC of the test module: `xx:xx:xx:xx:xx:xx` (host name `PicoTunerWH-XXXXXX`). Give it a DHCP
  reservation on the tuner's router (e.g. the old TUNER_IP), or read its lease there after
  the swap. The 9997 broadcast does not cross the router.
- No baseline with 0v3g beforehand: the dish has to move to reach the tuner, so the pointing
  changes anyway. 0v3g's beacon MER from the handoff (10.5 dB) is the only reference.

## On the day (`TUNER` = the module's address)
`tools/whlog.py` tunes RX1, logs every 500 ms status record to CSV and counts the TS rate. It takes
the receiver over: status and TS go to the machine running it. Re-send your own tune command after.

0. **Swap** with the tuner powered off. Power on: green LED "HI" in Morse, then blinking until the
   Ethernet link, then steady once DHCP worked. Check: `python3 tools/ptupdate.py TUNER --info`.
0b. **Dish back on QO-100** (SPID MD-03, rotctld ROTATOR_IP:4533, every move on your OK).
   Step 1 then shows how well it points: MER near 10.5 dB as before. If it is clearly lower, a
   small az/el raster around the position with step 5's AGC1/MER per point is the next test.
1. **Beacon, H** (locked, as in the handoff):
   `python3 tools/whlog.py TUNER --freq 10491500 --srate 1500 --vgx HI --duration 120 --csv fw/agc-test/1-beacon-H.csv`
   Expect `DVB-S2`, MER about 10.5 dB (handoff: 10.5 with 0v3g), every new field present,
   LDPC/BCH counts and BER low and steady.
2. **Empty frequency, H** (no lock, noise only):
   `python3 tools/whlog.py TUNER --freq 10485000 --vgx HI --duration 60 --csv fw/agc-test/2-empty-H.csv`
   Expect `search`, MER/BER/LDPC empty, AGC1/AGC2/power present and different from step 1.
3. **Beacon, V** (cross-polar):
   `python3 tools/whlog.py TUNER --freq 10491500 --vgx LO --duration 60 --csv fw/agc-test/3-beacon-V.csv`
4. **LNB supply off** (only the tuner's own noise; shows which way AGC1 moves):
   `python3 tools/whlog.py TUNER --freq 10491500 --vgx OFF --duration 30 --csv fw/agc-test/4-lnb-off.csv`, then step 1 again.
5. **AGC vs frequency** across the wideband transponder:
   `python3 tools/whlog.py TUNER --sweep 10485000:10500000:250 --dwell 3 --vgx HI --csv fw/agc-test/5-sweep.csv`
   AGC1 shows wide-band power, so expect a shape over the transponder, not the beacon alone.
6. **Stability**: step 1 for 10 minutes (`--duration 600`), in VLC too if wanted (`--no-ts`, VLC on
   `udp://@:9941`). Expect no gaps in the status, MER as in step 1, steady TS rate.
7. **Update while receiving**: keep step 6 running and, from another terminal,
   `python3 tools/ptupdate.py TUNER fw/testboard/picotunewh.bin`. Expect TS and status gaps during
   the upload and the reboot, whlog re-tuning after 10 s of silence, `trial` then `confirmed`.

Copy the CSVs back to the repository (`fw/agc-test/` is local, like the rest of `fw/`).

## What to look at afterwards
- AGC1 between beacon / empty / LNB-off: direction and range (LongMynd treats it as gain: higher =
  weaker input). This is the value for Sun and noise scans.
- POWER I/Q (1/4 of the ADC level) staying in range while AGC works.
- MER and error counters equal to 0v3g's: the extra reads every 500 ms must not cost reception.
- LDPC/BCH are chip totals (both demodulators): with RX2 idle they are RX1's.
