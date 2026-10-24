# Beam cross-sections before the feed adjustment (2026-10-24)

Dish 2.4 m prime focus, LNB SNF-031 on three threaded struts (12, 4 and 8 o'clock seen from behind
the dish), rotator SPID MD-03 with track_tolerance 0.08 (it stops up to 0.08 deg from the target).
Offsets are on the sky (az offset x cos el). Raw data in `fw/agc-test/` (not committed):
`xsec-2026-10-24*.csv`, `sun-lna-xsec-2026-10-24.csv`.

## QO-100 beacon (RX1, 1500 kS/s), AGC2 as power (noise only: about 590 at 1500 kS/s)
- Peak AGC2 142, MER 11.0-11.3 dB at az 169.05 / el 31.05 (QO-100 computed for the wrong QTH: az 168.55, el 30.85; see the correction at the end).
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

## Second opinion and the axial test (2026-10-24 afternoon)
A second analysis (Fable) pointed out an azimuth sign error in the notes above: when the peak moves
to a LOWER rotator azimuth, the beam has moved RIGHT of the dish axis (seen from behind), just as
a higher rotator elevation means the beam moved down. With that fixed, the strut model (beam
opposite to the feed shift, about 0.5 deg per cm) and the strut labels agree with adjustments 2
and 3; adjustment 1 differs because the strut anchors are not symmetric (equal struts do not mean
a centred feed; the flange-to-rim distances are the better check).

Figure of merit from now on: noise AGC2 (3 deg off) against peak AGC2, measured in one session
(`fw/agc-test/fom-2026-10-24.txt`). Peak search with 0.2 deg steps: with 0.1 deg steps the 0.08
deg rotator deadband makes every other step a no-move.
| state | aperture | peak AGC2 | noise/peak |
|---|---|---|---|
| C, clamp 12 mm in | ~938 mm | 146 (MER 11.0) | 7.93 dB |
| C, clamp back | ~950 mm | 145 | 7.85 dB |
The 12 mm axial change is within the measurement spread: the feed is near the focus, or the gain
is not sensitive to it in this range.

Azimuth cut at ~950 mm, el 33.7: peak AGC2 143 at rotator az 168.0; steep on the low-azimuth side
(194 at 167.0, 252 at 166.7); plateau 148-152 from 168.6 to 169.4, falling only beyond 169.6.
Next: lateral test, strut 8 o'clock +6 mm, 4 o'clock -6 mm (feed ~1 cm right seen from behind).

## The AGC2 noise reference was wrong (found 2026-10-24 ~15:15)
AGC2 is normalised by the LNA AGC and AGC1, which follow the total LNB power. The "noise 3 deg off
in azimuth" reference used for the noise/peak figures points at another satellite (very probably
Astra 2 at 28.2E, about 2.3 deg along the arc): there the automatic LNA VGO reads 4, on QO-100 15,
on cold sky 31. The noise/peak figures (7.85-8.01 dB) and the transponder hump test are withdrawn.
Beam shapes from MER and the peak shifts stand. From ota14 on, measurements use AGCHOLD=1 and a
cold-sky reference above the arc.

## End of 2026-10-24 (feed work paused)
- Final strut changes: lateral half step after the test step (8 o'clock +6 then +3 mm, 4 o'clock -6
  then -3 mm); aperture ~950 mm. Azimuth beam symmetric (peak 0.1 deg off the centre of the
  AGC2-185 span, width 2.59 deg rotator at that level, narrowest so far); elevation slightly
  broader downwards.
- QO-100 peak: rotator az ~169.2-169.3, el ~33.45 (computed for the wrong QTH: 168.55 / 30.85). One az centre scan
  from level crossings: 169.13. Rotator zero (az_pos, el_zero) NOT changed yet; changing el_zero
  by -2.6 also moves the physical place of el_min (-5), so el_min needs moving with it.
- Sun in the afternoon (az ~220, el ~17, partly behind trees): no response with AGCHOLD=1
  at 1500 kS/s; at 27.5 MS/s AGC2 does not respond at all (do not use 27.5 MS/s for this).

## Rotator recalibrated on the new beam centre (2026-10-24 ~16:40)
Beam centre from AGC2-180 level crossings, both scan directions: rotator az 169.14 (169.13 and
169.15), el 33.69 (33.70 and 33.67). True QO-100: az 168.55, el 30.85 geometric, 30.88 with
refraction; the rotator read az +0.59, el +2.81 too much. Applied via `\set_conf` (approved by
the owner), saved and the controller rebooted: az_pos 168.60 (dish at rest 0.05 east of the
centre), tilt_dir 58.149 -> 57.559, el_zero 6.980 -> 4.17, and, to keep the same physical
positions, el_min -5 -> -7.8, el_max 80 -> 77.2, park_el 0 -> -2.8, service_el 60 -> 57.2.
Check at the true QO-100 position: centre AGC2 149 (MER 10.3), az -0.4: 149, az +0.4: 152,
el -0.4: 156, el +0.4: 154, i.e. within about 0.1 deg. The feed-service position used on
2026-10-24 (shown -4) is now shown about -6.8. The first move near el_min should be watched.
Measurements run from the power host (Docker, host network) as the notebook was off the
station LAN.

## Correction: the station coordinates (2026-10-24 evening)
All QO-100, Sun and Moon directions above were computed for JO81LC, which is not where the dish
is. The dish is in JO81QO (height from GPS above the WGS84 ellipsoid, about 175 m above sea
level). From there QO-100 is at az 169.20, el 30.385 geometric, 30.41 with refraction. So the
rotator recalibration above (zero set for 168.55 / 30.88) leaves the rotator reading about
az 0.64 too low and el 0.47 too high; the beam centre in the old rotator angles (169.14 / 33.69)
means the rotator read az -0.06 and el +3.28 against the sky before that recalibration. The Sun
pointing of 2026-10-24 was off by a similar amount. Beam shapes, peak shifts and the feed work are
not affected.
Rotator corrected the same evening (owner's OK): at rest on QO-100 az_pos += 0.644, tilt_dir
57.559 -> 58.203, el_zero 4.17 -> 3.704, el_min -8.27, el_max 76.73, park_el -3.27, service_el
56.73 (physical positions unchanged), saved, controller rebooted. Check at the true QO-100
direction 169.20 / 30.41: centre AGC2 144 (MER 10.5), az -0.4: 144, az +0.4: 146, el -0.4: 150,
el +0.4: 149. The feed-service position is now shown about el -7.3.
