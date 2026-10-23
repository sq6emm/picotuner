# First run in the tuner, 2026-10-23 evening

W5500-EVB-Pico module with 0v3e-ota2 in the PicoTuner (FTS-4334LV), at TUNER_IP (no
reservation; found by sending INFO to the whole /24). Raw CSVs in `fw/agc-test/` (local).

## Status fields (steps 1-5 of `docs/agc-test.md`)
| condition | AGC1 | AGC2 | power I/Q | LNA |
|---|---|---|---|---|
| beacon 10491.5 H, locked | ~35 780 | ~177 | 143/140 | 115 |
| empty 10485 H | ~34 800 | ~390 | 140/135 | 115 |
| beacon frequency, V | ~34 830 | 360-430 | 140/136 | 116 |
| LNB supply off | 0 | ~2 100 | 24/23 | 127 |

- AGC1 rises with the input level (opposite of the "gain" reading suggested by LongMynd), wide-band
  (the tuner's whole filter). It is 0 and power I/Q falls to 24 with the LNB off: end of the AGC range.
- AGC2 is in-channel and relative to the band: ~177 with the beacon, ~390 on noise; higher next to
  the transponder (AGC1 cuts gain for the band's power). Usable as a carrier detector per slot.
- Sweep 10485-10500 MHz / 250 kHz at 1500 kS/s: the beacon 10490.75-10492.5; low AGC2 and `header`
  at 10496.25-10497.5 (a station at another symbol rate).
- `$11` BER is meaningless in DVB-S2 (1250, 1241, 397...): keep it for DVB-S only.
- `$23` BCH-uncorrected reads 1 with 0 BCH errors and good video: inverted.
- AGC1 at 10485 MHz vs symbol rate (tuner filter): SR 1500 mean 34 783 sd 184, 10000 35 667 sd 158,
  27500 36 525 sd 129. Very roughly 200-250 units per dB (from the filter width, not calibrated).

## Update over Ethernet: hangs with the NIM fitted
Three BEGINs, three hangs (power cycle each time): during a stream to the notebook, right after
boot without any command, and with both receivers switched off (freq=0). The metadata sector was
erased the first time, so the Core1 lockout and the erase completed and the hang came after. Not
seen in dozens of runs on the bare boards, which were always on USB. Suspects: `printf` with USB not
connected (core0 prints right after the erase), supply dip during the erase with the NIM drawing
several hundred mA. Fix planned: watchdog, no printf in the update path, cooperative Core1 pause with
timeouts, progress codes in watchdog scratch registers. Needs one USB install.

## Pointing (SPID MD-03 via rotctld ROTATOR_IP:4533)
- Smallest move ~0.5 deg (requests of 0.25 deg do nothing). rotctld's reply includes
  `el_inclinometer`, agreeing with the elevation reading.
- Beacon MER over az (el 30.44): 168.37 -> 8.0-8.5, 169.08 -> 10.05, 169.59 -> 8.4-8.6, 170.32 no lock.
  Over el (az 169.05): 29.40 no lock, 29.92 -> 4.6, 30.41 -> 9.2, 30.90 -> 10.75, 31.42 -> 10.5.
- Best QO-100: rotator az 169.05 el 31.05, MER 10.7-10.8 dB, LDPC errors ~30/read (was 210-270 at
  169.34/30.44, MER 8.7). Computed QO-100 (25.9 E) from JO81LC: az 168.55 el 30.85,
  so the rotator reads about az +0.5, el +0.2 deg against the sky.

## Moon (93 %, 0.53 deg), 10485 MHz, 27 500 kS/s, drift scans
- 1st, rotator at 141.32/33.69 before the offset was known, Moon there 20:32: nothing (30 s means
  within +-15 of 36 315). QO-100 direction itself read ~200 units higher than this cold sky.
- 2nd, with the offset, rotator 151.33/36.98, Moon there 21:06: AGC1 +20..+55 (mean ~+40) from
  21:04:00 to 21:10:30 over a 36 290 baseline (sd of 30 s means 9), back to baseline after 21:11.
  Timing and length (~6 min) fit a transit; two maxima (21:05, 21:09), probably off-centre.
- Expected 2-3 dB (T_moon ~230 K partly filling a 0.83 deg beam over a T_sys ~70-100 K); ~0.2 dB
  on the rough scale above. Either the scale is wrong, the Moon went through the beam's edge, or
  AGC1 is coarse for small noise changes. The Sun (more than 10 dB) is the next step: it calibrates
  AGC1 in dB and checks the pointing model.
