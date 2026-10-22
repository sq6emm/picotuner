"""Pack flat images into one RP2040 UF2 (family 0xe48bff56).
    python3 uf2pack.py out.uf2 ptboot.bin@0x10000000 picotunewh.bin@0x10008000
Gaps between the images are filled with 0xff blocks: with a gap, the RP2040 ROM's USB drive
left only the last 1 KiB of every app sector written (seen on a W5100S-EVB-Pico, Linux)."""
import struct
import sys

FAMILY = 0xE48BFF56
FLAG_FAMILY = 0x2000


def blocks(parts):
    for data, base in parts:
        for off in range(0, len(data), 256):
            chunk = data[off:off + 256]
            yield base + off, chunk + b'\0' * (256 - len(chunk))


def main():
    out, specs = sys.argv[1], sys.argv[2:]
    parts = []
    for spec in specs:
        path, base = spec.split('@')
        parts.append((open(path, 'rb').read(), int(base, 0)))
    ranges = sorted((b, b + len(d)) for d, b in parts)
    for (s1, e1), (s2, _) in zip(ranges, ranges[1:]):
        assert e1 <= s2, f'images overlap at {s2:#x}'
    filled = []
    for (data, base), (_, nxt) in zip(sorted(parts, key=lambda p: p[1]), sorted(parts, key=lambda p: p[1])[1:] + [(None, None)]):
        filled.append((data, base))
        end = base + len(data)
        if nxt is not None and nxt > end:
            pad_from = (end + 255) & ~255                     # last image block was zero-padded to 256
            data_end = data + b'\xff' * ((pad_from - end) if pad_from > end else 0)
            filled[-1] = (data_end, base)
            if nxt > pad_from:
                filled.append((b'\xff' * (nxt - pad_from), pad_from))
    payload = list(blocks(filled))
    with open(out, 'wb') as f:
        for n, (addr, chunk) in enumerate(payload):
            head = struct.pack('<8I', 0x0A324655, 0x9E5D5157, FLAG_FAMILY, addr, 256, n, len(payload), FAMILY)
            f.write(head + chunk + b'\0' * (476 - 256) + struct.pack('<I', 0x0AB16F30))
    print(f'{out}: {len(payload)} blocks, ' + ', '.join(f'{b:#x}-{e:#x}' for b, e in ranges))


if __name__ == '__main__':
    main()
