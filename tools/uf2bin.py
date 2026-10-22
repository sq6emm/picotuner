import struct, sys
d = open(sys.argv[1], 'rb').read()
blocks = []
for i in range(0, len(d), 512):
    b = d[i:i+512]
    m0, m1, fl, addr, sz, no, n, fam = struct.unpack('<8I', b[:32])
    assert m0 == 0x0A324655 and m1 == 0x9E5D5157, i
    blocks.append((addr, b[32:32+sz], fl, fam))
lo = min(a for a, *_ in blocks); hi = max(a + len(x) for a, x, *_ in blocks)
img = bytearray(b'\xff' * (hi - lo))
for a, x, *_ in blocks: img[a-lo:a-lo+len(x)] = x
print(f'blocks {len(blocks)} base {lo:#x} end {hi:#x} size {hi-lo} family {blocks[0][3]:#x} flags {blocks[0][2]:#x}')
open(sys.argv[2], 'wb').write(img)
