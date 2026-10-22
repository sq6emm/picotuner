"""Firmware update for PicoTuneWH (ptwh0v3e-ota) over Ethernet.

    python3 ptupdate.py TUNER_IP picotunewh.bin            # upload, reboot, wait for it
    python3 ptupdate.py TUNER_IP --info                    # update state only
    options: --port 9920 (base port + 20), --key KEY (built in with -DPTUP_KEY)

The image is the application linked for 0x10008000 (picotunewh.bin, or its .uf2). The tuner
stages it, ptboot installs it on the reboot and keeps the old one; the new one has to run for
60 s with an IP address, otherwise ptboot restores the old one after 3 boots.
Protocol: firmware/common/ptupdate.h."""
import argparse
import socket
import struct
import sys
import time
import zlib

APP_BASE = 0x10008000
APP_MAX = 0x78000
CMD_INFO, CMD_BEGIN, CMD_DATA, CMD_END, CMD_ABORT = 1, 2, 3, 4, 5
ERRORS = {1: 'state', 2: 'key', 3: 'size', 4: 'offset', 5: 'crc', 6: 'image', 7: 'flash', 8: 'format'}
CHUNK = 1024


def load_image(path):
    data = open(path, 'rb').read()
    if not path.endswith('.uf2'):
        return data
    image = {}
    for i in range(0, len(data), 512):
        m0, m1, _, addr, size = struct.unpack('<5I', data[i:i + 20])
        assert m0 == 0x0A324655 and m1 == 0x9E5D5157, 'not a UF2 file'
        image[addr] = data[i + 32:i + 32 + size]
    lo, hi = min(image), max(a + len(d) for a, d in image.items())
    if lo != APP_BASE:
        sys.exit(f'UF2 starts at {lo:#x}, the update needs an image linked for {APP_BASE:#x}')
    flat = bytearray(b'\xff' * (hi - lo))
    for a, d in image.items():
        flat[a - lo:a - lo + len(d)] = d
    return bytes(flat)


class Tuner:
    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq = 0
        self.resends = 0

    def request(self, cmd, body=b'', timeout=1.0, tries=8):
        self.seq = (self.seq + 1) & 0xFFFF
        packet = b'PTUP' + struct.pack('<BBH', cmd, 0, self.seq) + body
        for attempt in range(tries):
            self.resends += attempt > 0
            self.sock.sendto(packet, self.addr)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                self.sock.settimeout(max(0.01, deadline - time.monotonic()))
                try:
                    reply, _ = self.sock.recvfrom(2048)
                except socket.timeout:
                    break
                if len(reply) < 12 or reply[:4] != b'PTUP':
                    continue
                rcmd, status, seq, value = struct.unpack('<BBHI', reply[4:12])
                if rcmd == cmd | 0x80 and seq == self.seq:
                    return status, value, reply[12:].decode(errors='replace')
        raise TimeoutError(f'no reply to command {cmd} from {self.addr[0]}:{self.addr[1]}')


def check(status, value, what):
    if status:
        sys.exit(f'{what}: error {ERRORS.get(status, status)} (value {value})')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('host')
    ap.add_argument('image', nargs='?')
    ap.add_argument('--port', type=int, default=9920)
    ap.add_argument('--key', default='picotuner')
    ap.add_argument('--info', action='store_true')
    ap.add_argument('--no-wait', action='store_true', help='do not wait for the tuner to come back')
    a = ap.parse_intermixed_args()

    t = Tuner(a.host, a.port)
    status, value, text = t.request(CMD_INFO)
    print(f'tuner: {text} (max image {value} bytes)')
    if a.info or not a.image:
        return

    data = load_image(a.image)
    if len(data) > APP_MAX:
        sys.exit(f'image is {len(data)} bytes, max {APP_MAX}')
    sp, pc = struct.unpack('<II', data[0x100:0x108])
    if not (APP_BASE + 0x100 <= (pc & ~1) < APP_BASE + len(data)):
        sys.exit(f'reset vector {pc:#x} is outside {APP_BASE:#x}+: not linked for the update slot')
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f'image: {len(data)} bytes, CRC-32 {crc:08x}')

    key = a.key.encode()
    check(*t.request(CMD_BEGIN, struct.pack('<IIB', len(data), crc, len(key)) + key, timeout=3)[:2], 'begin')
    offset, started = 0, time.monotonic()
    while offset < len(data):
        chunk = data[offset:offset + CHUNK]
        status, value, _ = t.request(CMD_DATA, struct.pack('<IH', offset, len(chunk)) + chunk, timeout=1.5)
        if status == 4:                                   # the tuner wants another offset
            offset = value
            continue
        check(status, value, f'data at {offset}')
        offset = value
        print(f'\r{offset}/{len(data)} bytes', end='', flush=True)
    print(f'  ({len(data) / 1024 / (time.monotonic() - started):.1f} KiB/s, {t.resends} resends)')
    status, value, text = t.request(CMD_END, timeout=5, tries=3)
    check(status, value, 'end')
    print(f'tuner: {text}')
    if a.no_wait:
        return

    print('waiting for the tuner to come back', end='', flush=True)
    time.sleep(10)
    for _ in range(60):
        try:
            status, value, text = t.request(CMD_INFO, timeout=1, tries=1)
            print(f'\ntuner: {text}')
            print('it confirms itself after 60 s with an IP address; check again with --info')
            return
        except TimeoutError:
            print('.', end='', flush=True)
    sys.exit('\nno answer after 70 s; after 3 boots without confirming, ptboot restores the old firmware')


if __name__ == '__main__':
    main()
