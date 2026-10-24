# PicoTuner (WinterHill / Ethernet) — signal level

Goal: get the NIM's (Serit FTS-4334L: STV0910 + STV6120) AGC / power
readings out of the Ethernet PicoTuner, for Sun and noise scans of the
2.4 m dish (MER alone needs a lock).

The tuner runs an own build of the PicoTuneWH firmware, made from the published 0v3e
source (GPL v3, see Upstream below).

- `tools/uf2bin.py in.uf2 out.bin` — UF2 to a flat image (base 0x10000000).
- Ready-made firmware images (W5500 and W5100S, USB first install and Ethernet update):
  https://github.com/sq6emm/picotuner/releases
- `firmware/` — own build of G4EWJ's 0v3e source (GPL v3): DHCP option 54, AGC/power/LNA in the
  status, firmware update over Ethernet (`ptboot` bootloader). See `docs/firmware.md`;
  `tools/ptupdate.py` uploads, `tools/uf2pack.py` packs UF2s, `firmware/test/run.sh` tests.
- `tools/whlog.py` — tune a receiver and log its status (AGC, MER, errors) to CSV, or sweep AGC
  over frequency. Plan for the first test in the tuner: `docs/agc-test.md`.
- `tools/qo100scan.py` — band scan run by the tuner itself (0v3e-ota3, fast AGC2-screened scan
  since 0v3e-ota13), prints the stations found.
- `web/qo100web/` — QO-100 WB Monitor: live scan list, a spectrum of the decoded stations and
  the chosen station as a low-bitrate HLS stream, running next to the tuner (Docker). Live at
  https://sdr.hamradio.pl/picotuner/.

## Upstream

The firmware in `firmware/` started from the PicoTuneWH 0v3e source released on the BATC wiki;
there is no upstream git repository, so this one cannot be a GitHub fork.

- PicoTuner Ethernet interface (firmware downloads, setup, commands):
  https://wiki.batc.org.uk/PicoTuner_Ethernet_Interface
- Source, PicoTuneWH 0v3e (GPL v3): https://wiki.batc.org.uk/images/2/21/Ptwh0v3e-source.zip
- Original firmware images: 0v3e
  https://wiki.batc.org.uk/images/d/d6/PicoTuneWH_ptwh0v3e_UF2.zip and 0v3g (DHCP fix)
  https://wiki.batc.org.uk/images/f/f9/PicoTuneWH_xwh0v3g_UF2.zip
- PicoTuner hardware: https://wiki.batc.org.uk/PicoTuner
- BATC forum thread: https://forum.batc.org.uk/viewtopic.php?f=15&t=8934
- WIZnet RP2040-HAT-C SDK used by the build: https://github.com/Wiznet/RP2040-HAT-C

The changes made here are described in `docs/firmware.md` (0v3e-ota1 .. ota15).
