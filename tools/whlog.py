"""Tune a PicoTuneWH receiver and log its status ($n,value on UDP 9901) to CSV.

    python3 whlog.py <tuner> --freq 10491500 --srate 1500 --vgx HI --duration 120 --csv beacon.csv
    python3 whlog.py IP --sweep 10485000:10499000:250 --dwell 3 --csv sweep.csv     # AGC vs frequency

Frequencies in kHz as in [to@wh] commands (offset 9750000 = LNB LO). The tuner sends status and TS to
whoever sent the last command, so this takes the receiver over (re-send your own tune command after).
The command is repeated every --resend s (the firmware stops sending off-net after 900 s) and after
10 s of silence (a reboot or an update). The TS on 9940+rx is only counted (kbit/s), use --no-ts to
leave that port to VLC.

Fields (firmware 0v3e-ota2): $1 state, $6 frequency, $9 SR, $12 MER, $18 MODCOD, $35 AGC1, $36 AGC2,
$4/$5 power I/Q, $2 LNA gain, $11 BER, $10 Viterbi, $21 LDPC, $22 BCH, $23 BCH uncorrected,
$7/$8 constellation I/Q, $28 TS status."""
import argparse
import csv
import select
import socket
import sys
import time

FIELDS = [('state', 1), ('freq_mhz', 6), ('sr', 9), ('mer_db', 12), ('modcod', 18), ('agc1', 35),
          ('agc2', 36), ('power_i', 4), ('power_q', 5), ('lna', 2), ('ber', 11), ('viterbi', 10),
          ('ldpc', 21), ('bch', 22), ('bch_unc', 23), ('const_i', 7), ('const_q', 8), ('ts_status', 28)]


def parse(packet):
    values = {}
    for line in packet.decode(errors='replace').splitlines():
        if line.startswith('$') and ',' in line:
            n, _, v = line[1:].partition(',')
            if n.isdigit():
                values[int(n)] = v.strip('\x00').strip()
    return values


class Logger:
    def __init__(self, a):
        self.a = a
        self.cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.status.bind(('', a.base + 1))
        self.ts = None
        if not a.no_ts:
            self.ts = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.ts.bind(('', a.base + 40 + a.rx))
        self.ts_bytes = 0
        self.last_status = 0.0
        self.last_cmd = 0.0
        self.writer = None
        if a.csv:
            self.file = open(a.csv, 'w', newline='')
            self.writer = csv.writer(self.file)
            self.writer.writerow(['time', 'set_freq_khz'] + [f for f, _ in FIELDS] + ['ts_kbps'])

    def tune(self, freq, quiet=False):
        c = (f'[to@wh] rcv={self.a.rx} freq={freq} offset={self.a.offset} srate={self.a.srate} '
             f'fplug={self.a.fplug} vgx={self.a.vgx}')
        self.cmd.sendto(c.encode(), (self.a.tuner, self.a.base + 20))
        self.last_cmd = time.monotonic()
        if not quiet:
            print(f'> {c}')

    def run(self, freq, seconds, quiet=False, settle=0.0):
        """log for `seconds`, return the rows received after `settle` s"""
        start = time.monotonic()
        rows, end, t_ts, last_print = [], start + seconds, start, 0.0
        while time.monotonic() < end:
            now = time.monotonic()
            if now - self.last_cmd >= self.a.resend or (now - max(self.last_status, self.last_cmd) >= 10):
                self.tune(freq, quiet)
            socks = [self.status] + ([self.ts] if self.ts else [])
            ready, _, _ = select.select(socks, [], [], 0.5)
            for s in ready:
                data, _ = s.recvfrom(65536)
                if s is self.ts:
                    self.ts_bytes += len(data)
                    continue
                v = parse(data)
                if v.get(0) != str(self.a.rx):
                    continue
                self.last_status = time.monotonic()
                dt = max(1e-3, self.last_status - t_ts)
                kbps = self.ts_bytes * 8 / dt / 1000
                self.ts_bytes, t_ts = 0, self.last_status
                row = [time.strftime('%H:%M:%S'), freq] + [v.get(n, '') for _, n in FIELDS] + [f'{kbps:.0f}']
                if self.last_status - start < settle:
                    continue
                rows.append(row)
                if self.writer:
                    self.writer.writerow(row)
                    self.file.flush()
                if not quiet and time.monotonic() - last_print >= 2:
                    last_print = time.monotonic()
                    d = dict(zip(['time', 'set'] + [f for f, _ in FIELDS] + ['ts_kbps'], row))
                    print(f"{d['time']} {d['state']:>7} MER {d['mer_db']:>5} AGC1 {d['agc1']:>6} AGC2 {d['agc2']:>6}"
                          f" P {d['power_i']}/{d['power_q']} LNA {d['lna']} BER {d['ber']} LDPC {d['ldpc']}"
                          f" BCH {d['bch']}/{d['bch_unc']} {d['modcod']} TS {d['ts_kbps']} kb/s")
        return rows


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('tuner')
    ap.add_argument('--rx', type=int, default=1)
    ap.add_argument('--freq', type=int, default=10491500, help='kHz')
    ap.add_argument('--offset', type=int, default=9750000, help='kHz (LNB LO)')
    ap.add_argument('--srate', type=int, default=1500, help='kS/s')
    ap.add_argument('--fplug', default='A')
    ap.add_argument('--vgx', default='HI', help='LNB supply: HI (18 V, H) / LO (13 V, V) / HIT / LOT / OFF')
    ap.add_argument('--duration', type=float, default=60)
    ap.add_argument('--sweep', help='start:stop:step in kHz')
    ap.add_argument('--dwell', type=float, default=3, help='s per sweep point')
    ap.add_argument('--settle', type=float, default=1.5, help='s ignored after each sweep step')
    ap.add_argument('--csv')
    ap.add_argument('--base', type=int, default=9900)
    ap.add_argument('--resend', type=float, default=300)
    ap.add_argument('--no-ts', action='store_true')
    a = ap.parse_args()

    log = Logger(a)
    if not a.sweep:
        log.tune(a.freq)
        rows = log.run(a.freq, a.duration)
        print(f'{len(rows)} status records' + (f' -> {a.csv}' if a.csv else ''))
        if not rows:
            sys.exit('no status received: tuner address, firewall (UDP 9901 in), or the receiver is not active')
        return

    start, stop, step = (int(x) for x in a.sweep.split(':'))
    print(f'{"freq kHz":>10} {"AGC1":>7} {"AGC2":>7} {"PWR I/Q":>9}  state')
    for f in range(start, stop + 1, step):
        log.tune(f, quiet=True)
        rows = log.run(f, a.dwell + a.settle, quiet=True, settle=a.settle)
        agc1 = [int(r[2 + 5]) for r in rows if r[2 + 5].lstrip('-').isdigit()]
        agc2 = [int(r[2 + 6]) for r in rows if r[2 + 6].lstrip('-').isdigit()]
        pi = [r[2 + 7] for r in rows if r[2 + 7]]
        pq = [r[2 + 8] for r in rows if r[2 + 8]]
        st = rows[-1][2] if rows else 'no status'
        print(f'{f:>10} {median(agc1) or "":>7} {median(agc2) or "":>7} {(median(pi) or "")+"/"+(median(pq) or ""):>9}  {st}')


if __name__ == '__main__':
    main()
