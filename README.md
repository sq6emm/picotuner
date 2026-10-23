# PicoTuner (WinterHill / Ethernet) — signal level

Goal: get the NIM's (Serit FTS-4334L: STV0910 + STV6120) AGC / power
readings out of the Ethernet PicoTuner, for Sun and noise scans of the
2.4 m dish (MER alone needs a lock).

The tuner runs an own build of the PicoTuneWH firmware, made from the published 0v3e
source (GPL v3, see Upstream below).

- `tools/uf2bin.py in.uf2 out.bin` — UF2 to a flat image (base 0x10000000).
- `firmware/` — own build of G4EWJ's 0v3e source (GPL v3): DHCP option 54, AGC/power/LNA in the
  status, firmware update over Ethernet (`ptboot` bootloader). See `docs/firmware.md`;
  `tools/ptupdate.py` uploads, `tools/uf2pack.py` packs UF2s, `firmware/test/run.sh` tests.
- `tools/whlog.py` — tune a receiver and log its status (AGC, MER, errors) to CSV, or sweep AGC
  over frequency. Plan for the first test in the tuner: `docs/agc-test.md`.
- `tools/qo100scan.py` — band scan run by the tuner itself (0v3e-ota3), prints the stations found.
