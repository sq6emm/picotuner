"""Band scan with the scanner inside PicoTuneWH 0v3e-ota13 (SCANF= / SCANSR= in a tune command).

    python3 qo100scan.py TUNER_IP                  # QO-100 WB plan on RX2
    python3 qo100scan.py TUNER_IP --rx 1,2         # the plan split over both receivers
    python3 qo100scan.py TUNER --group 10491500:1500 --csv found.csv

The default plan follows the BATC QO-100 WB bandplan (wiki.batc.org.uk/QO-100_WB_Bandplan), as in
the original scanner.sh: wide channels 10493.25/10494.75/10496.25 MHz at 1500 and 1000 kS/s,
narrow channels every 500 kHz from 10492.75 to 10499.25 MHz at 333 kS/s, and 500 kS/s only up to
10496.75 MHz (narrow channels 1-9). Each pass screens every channel by AGC2 and tries a lock only
where a carrier shows; the service name is collected on every 10th pass.

The tuner steps through the channels itself and sends one line per station found to UDP 9905 of
this machine; this tool only starts it, prints the lines and repeats the command every 5 minutes
(the firmware stops sending off-net after 15). With one receiver and several groups, the groups
take turns, one pass each. Ctrl-C switches the scanning receivers off (freq=0), or tunes them to
--park FREQ:SR. While it runs, the receivers' status and TS also come to this machine.

A group is FREQS:SRS, FREQS as "start-stop/step" or "f1,f2,...", SRS as "sr1,sr2" (kHz, kS/s).
Several frequency lists with their own symbol rates go in one group separated by ';' on both sides,
e.g. "10493250,10494750:1500,1000;10497250-10499250/500:333"."""
import argparse
import csv
import select
import socket
import sys
import time

WIDE = '10493250,10494750,10496250'
QO100_ONE = [f'{WIDE};10492750-10496750/500;10497250-10499250/500:1500,1000;500,333;333']
QO100_TWO = [f'{WIDE};10492750-10494750/500:1500,1000;500,333',              # RX1: about half
             '10495250-10496750/500;10497250-10499250/500:500,333;333']        # RX2: the rest


def parse_fields(line):
    out = {}
    for part in line.split():
        if '=' in part:
            k, _, v = part.partition('=')
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('tuner')
    ap.add_argument('--rx', default='2', help='receiver(s), e.g. 2 or 1,2')
    ap.add_argument('--group', action='append', help='FREQS:SRS, repeatable (default: QO-100 WB plan)')
    ap.add_argument('--offset', type=int, default=9750000)
    ap.add_argument('--vgx', default='HI')
    ap.add_argument('--fplug', default='A')
    ap.add_argument('--wait', type=int, help='ms to wait for a lock (default by SR in the firmware)')
    ap.add_argument('--base', type=int, default=9900)
    ap.add_argument('--resend', type=float, default=300)
    ap.add_argument('--park', help='FREQ:SR to tune to on exit instead of switching off')
    ap.add_argument('--csv')
    ap.add_argument('--quiet', action='store_true', help='print empty passes only every 20th')
    a = ap.parse_args()

    rxs = [int(x) for x in a.rx.split(',')]
    groups = a.group or (QO100_TWO if len(rxs) >= 2 else QO100_ONE)
    cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    res = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    res.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    res.bind(('', a.base + 5))
    writer = None
    if a.csv:
        f = open(a.csv, 'a', newline='')
        writer = csv.writer(f)

    def send(rx, text):
        cmd.sendto(text.encode(), (a.tuner, a.base + 20))

    def start(rx, group):
        freqs, srs = group.split(':')
        text = (f'[to@wh] rcv={rx} offset={a.offset} fplug={a.fplug} vgx={a.vgx} '
                f'scanf={freqs} scansr={srs}' + (f' scanms={a.wait}' if a.wait else ''))
        send(rx, text)
        print(f'{time.strftime("%H:%M:%S")} RX{rx} scan {freqs} @ {srs} kS/s')

    # one group per receiver when there are enough, otherwise the groups take turns
    turn = {rx: i % len(groups) for i, rx in enumerate(rxs)}
    rotate = len(groups) > len(rxs)
    last = {}
    for rx in rxs:
        start(rx, groups[turn[rx]])
        last[rx] = time.monotonic()
    try:
        while True:
            if select.select([res], [], [], 1.0)[0]:
                data, _ = res.recvfrom(4096)
                for line in data.decode(errors='replace').replace('\x00', '').splitlines():
                    d = parse_fields(line)
                    rx = int(d.get('rx', 0))
                    stamp = time.strftime('%Y-%m-%d %H:%M:%S')
                    if line.startswith('[from@wh:SCAN]'):
                        name = line.split('name=')[1].split(' provider=')[0] if 'name=' in line else ''
                        print(f"{stamp} RX{rx} {d.get('freq')} {d.get('srate'):>5} {d.get('state')} "
                              f"MER {d.get('mer')} {line.split('modcod=')[1].split(' lock=')[0]} "
                              f"lock {d.get('lock')} ms AGC2 {d.get('agc2')}  {name}")
                        if writer:
                            writer.writerow([stamp, rx, d.get('freq'), d.get('srate'), d.get('state'), d.get('mer'),
                                             line.split('modcod=')[1].split(' lock=')[0], d.get('lock'), name,
                                             d.get('agc1')])
                            f.flush()
                    elif line.startswith('[from@wh:SCANPASS]'):
                        if not a.quiet or d.get('found') != '0' or int(d.get('pass', 0)) % 20 == 1:
                            print(f"{stamp} RX{rx} pass {d.get('pass')}: {d.get('found')} found, "
                                  f"{int(d.get('time', 0)) / 1000:.1f} s (screen {d.get('screen')} ms, "
                                  f"{d.get('candidates')} candidates)")
                        if rotate and rx in turn:
                            turn[rx] = (turn[rx] + len(rxs)) % len(groups)
                            start(rx, groups[turn[rx]])
                            last[rx] = time.monotonic()
            for rx in rxs:
                if time.monotonic() - last[rx] >= a.resend:
                    start(rx, groups[turn[rx]])
                    last[rx] = time.monotonic()
    except KeyboardInterrupt:
        for rx in rxs:
            if a.park:
                freq, sr = a.park.split(':')
                send(rx, f'[to@wh] rcv={rx} freq={freq} offset={a.offset} srate={sr} fplug={a.fplug} vgx={a.vgx}')
            else:
                send(rx, f'[to@wh] rcv={rx} freq=0 offset={a.offset} srate=1500 fplug={a.fplug}')
        print('\nstopped')


if __name__ == '__main__':
    main()
