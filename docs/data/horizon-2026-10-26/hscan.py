# Noise horizon scan: RX2 10501 MHz 1500 kS/s, AGCHOLD=2 set on cold sky (az 180 el 60), then full-speed
# az sweeps 5..355 (serpentine) at each elevation; every AGC stream sample is logged with the rotator
# position polled ~5x/s.  hscan.py out.csv [rows]  (run on the station LAN: off-net streams stop after 15 min)
import csv, select, socket, sys, threading, time
ROWS = [0, 3, 6, 10, 15, 20, 30, 45]
if len(sys.argv) > 2:                         # "6,10,15" or "0:60" (1 deg steps)
    a = sys.argv[2]
    ROWS = list(range(int(a.split(':')[0]), int(a.split(':')[1]) + 1)) if ':' in a else [float(x) for x in a.split(',')]
REFEVERY = 10                                 # rows between cold-sky reference visits (hold drift)
REF = (180.0, 60.0)
cur = [0.0, 0.0, 0.0]
def cmd(c):                                   # commands on their own connection; the reply is not needed
    s = socket.create_connection(('192.168.253.123', 4533), timeout=5); s.sendall((c + '\n').encode())
    time.sleep(0.2); s.close()
def fl(t):
    try: return float(t)
    except ValueError: return None
def poller():                                 # 'p' on a persistent connection; the firmware may append status lines
    ps = socket.create_connection(('192.168.253.123', 4533), timeout=5)
    while True:
        try:
            ps.sendall(b'p\n'); time.sleep(0.15); buf = b''
            while select.select([ps], [], [], 0.05)[0]:
                d = ps.recv(4096)
                if not d: raise OSError('closed')
                buf += d
            v = [fl(x) for x in buf.decode(errors='replace').split('\n')]
            for i in range(len(v) - 1):
                if v[i] is not None and v[i + 1] is not None:
                    cur[:] = [time.time(), v[i], v[i + 1]]; break
        except Exception as x:
            print('poll', x, flush=True); time.sleep(1)
            ps = socket.create_connection(('192.168.253.123', 4533), timeout=5)
def goto(az, el, tol=0.5):                   # the rotator can stop ~0.35 deg short in el
    cmd(f'P {az:.2f} {el:.2f}'); t0 = tl = time.time()
    while time.time() - t0 < 400:
        time.sleep(0.5)
        if abs(cur[1] - az) <= tol and abs(cur[2] - el) <= tol: return
        if time.time() - tl > 5 and (abs(cur[1] - az) > tol or abs(cur[2] - el) > tol): cmd(f'P {az:.2f} {el:.2f}'); tl = time.time()
    print('goto timeout', az, el, cur, flush=True)
r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); r.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); r.bind(('', 9906))
c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
fh = open(sys.argv[1], 'w', newline=''); w = csv.writer(fh)
w.writerow(['t', 'row', 'az', 'el', 'agc2', 'pi', 'hold'])
def log(row, until, kick=None):                # kick: resent every 5 s (a command after a direction change can be ignored)
    while select.select([r], [], [], 0)[0]: r.recv(4096)
    tk = time.time()
    while not until():
        if kick and time.time() - tk > 5: cmd(kick); tk = time.time()
        if select.select([r], [], [], 0.3)[0]:
            for l in r.recv(4096).decode(errors='replace').replace('\x00', '').splitlines():
                d = dict(p.split('=') for p in l.split()[1:] if '=' in p)
                if d.get('rx') == '2':
                    w.writerow([round(time.time(), 2), row, cur[1], cur[2], d.get('agc2'), d.get('pi'), d.get('hold')])
    fh.flush()
threading.Thread(target=poller, daemon=True).start(); time.sleep(1)
goto(*REF)
c.sendto(b'[to@wh] rcv=2 freq=10501000 offset=9750000 srate=1500 fplug=A vgx=HI agcms=100 agchold=2', ('192.168.253.193', 9920))
time.sleep(4)
c.sendto(b'[to@wh] rcv=1 freq=0 offset=9750000 srate=1500 fplug=A', ('192.168.253.193', 9920))
t1 = time.time(); log('ref', lambda: time.time() - t1 > 20)
for i, el in enumerate(ROWS):
    if i and i % REFEVERY == 0:
        goto(*REF); t1 = time.time(); log('ref', lambda: time.time() - t1 > 20)
    legs = [5.0, 95.0, 185.0, 275.0, 355.0][::1 if i % 2 == 0 else -1]   # < 180 deg legs: no short-way choice
    goto(legs[0], el); time.sleep(1)
    for a1 in legs[1:]:
        cmd(f'P {a1:.2f} {el:.2f}')
        log(el, lambda: abs(cur[1] - a1) <= (0.5 if a1 == legs[-1] else 3.0), f'P {a1:.2f} {el:.2f}')
    print(time.strftime('%H:%M:%S'), 'row', el, 'done', flush=True)
goto(*REF); t1 = time.time(); log('ref', lambda: time.time() - t1 > 20)
c.sendto(b'[to@wh] rcv=2 freq=0 offset=9750000 srate=1500 fplug=A', ('192.168.253.193', 9920))
goto(169.29, 30.38); print('back at', cur, flush=True)
