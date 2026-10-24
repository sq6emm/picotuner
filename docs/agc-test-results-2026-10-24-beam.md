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
