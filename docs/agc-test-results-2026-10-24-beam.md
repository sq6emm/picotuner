# Beam cross-sections before the feed adjustment (2026-10-24)

Dish 2.4 m prime focus, LNB SNF-031 on three threaded struts (12, 4 and 8 o'clock seen from behind
the dish), rotator SPID MD-03 with track_tolerance 0.08 (it stops up to 0.08 deg from the target).
Offsets are on the sky (az offset x cos el). Raw data in `fw/agc-test/` (not committed):
`xsec-2026-10-24*.csv`, `sun-lna-xsec-2026-10-24.csv`.

## QO-100 beacon (RX1, 1500 kS/s), AGC2 as power (noise only: about 590 at 1500 kS/s)
- Peak AGC2 142, MER 11.0-11.3 dB at az 169.05 / el 31.05 (true QO-100: az 168.55, el 30.85).
- az +: -3 dB near +0.6 deg, null near +0.8 deg (only about 8.5 dB down), sidelobe after +1.0.
- az -: broad shoulder, about 2.7 dB down at -1.2 deg, no null within the scan.
- el +: null near +1.0..+1.1 deg; el -: shoulder like az -, no null to -1.2 deg.
- Same in both scan directions (differences about 0.1 deg: backlash and deadband).

## Sun (RX2, 27.5 MS/s at 10485 MHz), az 164-168, el 26
AGC1 and AGC2 do not see the Sun: the STVVGLNA AGC takes the whole rise. The LNA VGO (`$2` & 31)
does: about 28 off the Sun, 5-7 on it (on/off cycles: 6.3/29.0, 6.7/29.0, 5.2/27.0). The VGO
step size in dB is not known yet.
- el cut: VGO <= 7 from -0.75 to 0 deg, 10.6 at -1.0, 16 at -1.25; on the + side 16.7 at +0.5,
  27 at +1.0. Broad downward, steep upward, as on the beacon.
- az cut: steep on the + side (24 at +0.5); strong response down to -1.25 deg on the - side,
  not monotonic (5 at -1.0, 14-16 at -0.75..-0.25, 7.9 at 0).
Earlier AGC1-only Sun/Moon results (2026-10-23, 2026-10-24 morning) need repeating with the LNA.

## Rotator note
A command right after a direction change was sometimes ignored (no move for 60 s, the same command
again moved it in 2 s); scripts now resend after 3 s.

## Correction to the beacon analysis above
The "shallow null, about 8.5 dB" used AGC2 590 as the noise level, measured at another frequency.
At the beacon frequency the noise level is about 360, so the nulls before the adjustment were
close to the noise floor. That point is withdrawn.

## Feed geometry and the first adjustment (2026-10-24, about 12:15)
Measured: horn aperture (flush with the corrugated flange) 920 mm from the dish centre (focus
962 mm from photogrammetry). Flange edge to dish outer rim along the struts, seen from behind:
12 o'clock 136 cm, 8 o'clock 128 cm, 4 o'clock 126.5 cm, i.e. the feed about 64 mm low and
about 9.5 mm right. Strut lengths 105 / 99.5 (4 o'clock) / 97.5 (8 o'clock) cm agree on the
vertical offset (top strut 4.3 cm longer than the mean, 4.35 cm predicted), not on left/right.
Adjustment: all three struts set to 100.7 cm (top -4.3, 4 o'clock +1.2, 8 o'clock +3.2 cm).
Whether the feed was also moved axially in the clamp: not recorded.

Result: the beam moved down about 2.6 deg and left about 1 deg; QO-100 peak now at rotator
az 167.9 / el 33.65 (was 169.05 / 31.05), MER 11.0 at the peak as before.
- el cut: nearly symmetric (MER about 7.5 at -0.8 and +0.6 deg, nulls at about -1.2 and +0.8).
- az cut: asymmetry reversed: steep to the left (no lock at -1.2), broad to the right
  (MER 6.9 at +1.2, 5.0 at +1.4). The unequal bottom strut changes moved the feed about
  1.7-1.9 cm right; next step: 4 o'clock +5.5 mm, 8 o'clock -5.5 mm (about 1 cm left).
Data: `fw/agc-test/xsec-after1-2026-10-24.csv`, `xsec-after2-2026-10-24.csv`.
