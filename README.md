# PicoTuner (WinterHill / Ethernet) — signal level

Goal: get the NIM's (Serit FTS-4334L: STV0910 + STV6120) AGC / power
readings out of the Ethernet PicoTuner, for Sun and noise scans of the
2.4 m dish (MER alone needs a lock).

The tuner runs an own build of the PicoTuneWH firmware, made from the published 0v3e
source (GPL v3, see Upstream below).

- `tools/uf2bin.py in.uf2 out.bin` — UF2 to a flat image (base 0x10000000).
