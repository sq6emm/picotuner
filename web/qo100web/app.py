"""QO-100 web monitor for a PicoTuner (PicoTuneWH 0v3e-ota16 or later).

Runs next to the tuner on the LAN, behind any reverse proxy and under any path (all URLs are
relative). It drives the tuner over UDP:
  - scanning: the in-firmware band scan (SCANF=/SCANSR=) on the QO-100 WB bandplan; with nobody
    watching both receivers share the plan, while someone watches RX2 scans it alone;
  - viewing: RX1 tuned to the chosen station, its TS (UDP base+41) re-encoded by ffmpeg to a small
    HLS stream (480p, ~400 kb/s by default); it stops IDLE_STOP_S after the last viewer leaves.
Anyone can watch and choose a station (one change per CHANGE_GAP_S for everybody).

Environment: TUNER (IP), HTTP_PORT (8090), BASE_PORT (9900), LNB_OFFSET (9750000),
VGX (HI), VIDEO_HEIGHT (480), VIDEO_KBPS (400), IDLE_STOP_S (60), HLS_DIR (/hls),
CHANGE_GAP_S (15).
"""
import asyncio
import json
import os
import shutil
import socket
import time
from pathlib import Path

from aiohttp import web

TUNER = os.environ.get('TUNER', '')                 # the tuner's IP address, required
HTTP_PORT = int(os.environ.get('HTTP_PORT', '8090'))
BASE = int(os.environ.get('BASE_PORT', '9900'))
OFFSET = int(os.environ.get('LNB_OFFSET', '9750000'))
VGX = os.environ.get('VGX', 'HI')
HEIGHT = int(os.environ.get('VIDEO_HEIGHT', '480'))
KBPS = int(os.environ.get('VIDEO_KBPS', '400'))
IDLE_STOP_S = int(os.environ.get('IDLE_STOP_S', '60'))
HLS_DIR = Path(os.environ.get('HLS_DIR', '/hls'))
CHANGE_GAP_S = int(os.environ.get('CHANGE_GAP_S', '15'))
STATIC = Path(__file__).parent / 'static'

RESEND_S = 240            # the firmware stops sending off-net after 15 min; resend well before
STATION_TTL_S = 30        # a station not reported for this long is dropped from the list
VIEWER_TTL_S = 12         # a page that stopped polling is no longer a viewer

# QO-100 WB bandplan (wiki.batc.org.uk/QO-100_WB_Bandplan): beacon + wide channels at 1500/1000,
# narrow channels every 500 kHz at 333 kS/s, 500 kS/s only up to 10496.75 MHz.
WIDE = '10491500,10493250,10494750,10496250'
PLAN_ONE = f'{WIDE};10492750-10496750/500;10497250-10499250/500:1500,1000;500,333;333'
PLAN_TWO = {1: f'{WIDE};10492750-10494750/500:1500,1000;500,333',
            2: '10495250-10496750/500;10497250-10499250/500:500,333;333'}


class State:
    def __init__(self):
        self.stations = {}         # freq -> dict
        self.status = {1: {}, 2: {}}
        self.viewing = None        # {'freq':, 'srate':, 'since':}
        self.viewers = {}          # client id -> last poll time
        self.last_viewer = 0.0
        self.passes = {1: None, 2: None}
        self.mode = None
        self.ffmpeg = None
        self.last_change = 0.0


S = State()
cmd_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def send(text):
    cmd_sock.sendto(text.encode(), (TUNER, BASE + 20))


def scan_cmd(rx, plan):
    freqs, srs = plan.split(':')
    send(f'[to@wh] rcv={rx} offset={OFFSET} fplug=A vgx={VGX} scanf={freqs} scansr={srs}')


def view_cmd(freq, srate):
    send(f'[to@wh] rcv=1 freq={freq} offset={OFFSET} srate={srate} fplug=A vgx={VGX}')


def apply_mode():
    """(Re)send the tuner commands for the current mode."""
    if S.viewing:
        view_cmd(S.viewing['freq'], S.viewing['srate'])
        scan_cmd(2, PLAN_ONE)
        S.mode = 'view'
    else:
        scan_cmd(1, PLAN_TWO[1])
        scan_cmd(2, PLAN_TWO[2])
        S.mode = 'scan'


# ---------------------------------------------------------------------------------------------
# UDP from the tuner

def fields_between(line, key, nxt):
    if key not in line:
        return ''
    rest = line.split(key, 1)[1]
    return rest.split(nxt, 1)[0] if nxt and nxt in rest else rest


class ScanProtocol(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        if addr[0] != TUNER:                             # only the tuner: anything else could inject into the page
            return
        for line in data.decode(errors='replace').replace('\x00', '').splitlines():
            try:
                self.line(line)
            except (ValueError, KeyError, IndexError):
                pass                                     # a malformed line is dropped

    def line(self, line):
        kv = {p.split('=', 1)[0]: p.split('=', 1)[1] for p in line.split() if '=' in p}
        if line.startswith('[from@wh:SCAN]'):
            freq = int(kv.get('freq', 0))
            st = S.stations.get(freq, {})
            name = fields_between(line, ' name=', ' provider=').strip()
            st.update(freq=freq, srate=int(kv.get('srate', 0)), state=kv.get('state', ''),
                      mer=kv.get('mer', ''), modcod=fields_between(line, ' modcod=', ' lock=').strip(),
                      agc2=kv.get('agc2', ''), rx=int(kv.get('rx', 0)), seen=time.time())
            if name:
                st['name'] = name
                st['provider'] = fields_between(line, ' provider=', ' agc1=').strip()
            for k in ('video', 'audio'):                 # ota15+, known once the TS showed them
                if kv.get(k):
                    st[k] = kv[k]
            st['d'] = decode_margin(st['mer'], st['modcod'])
            st['capacity'] = capacity_kbps(st['srate'], st['modcod'], st['state'])
            st.setdefault('first', time.time())
            S.stations[freq] = st
        elif line.startswith('[from@wh:SCANPASS]'):
            rx = int(kv.get('rx', 0))
            S.passes[rx] = {'time': int(kv.get('time', 0)), 'found': int(kv.get('found', 0)),
                            'screen': int(kv.get('screen', 0)), 'candidates': int(kv.get('candidates', 0)),
                            'at': time.time()}


# MER needed to decode each MODCOD (tenths of dB), as in the firmware's modinfo tables
MIN_MER_S2 = {'QPSK 1/4': -23, 'QPSK 1/3': -12, 'QPSK 2/5': -3, 'QPSK 1/2': 10, 'QPSK 3/5': 22,
              'QPSK 2/3': 31, 'QPSK 3/4': 40, 'QPSK 4/5': 47, 'QPSK 5/6': 52, 'QPSK 8/9': 62,
              'QPSK 9/10': 64, '8PSK 3/5': 55, '8PSK 2/3': 66, '8PSK 3/4': 79, '8PSK 5/6': 94,
              '8PSK 8/9': 107, '8PSK 9/10': 110, '16APSK 2/3': 90, '16APSK 3/4': 102,
              '16APSK 4/5': 110, '16APSK 5/6': 116, '16APSK 8/9': 129, '16APSK 9/10': 131,
              '32APSK 3/4': 127, '32APSK 4/5': 136, '32APSK 5/6': 143, '32APSK 8/9': 157,
              '32APSK 9/10': 160}


def decode_margin(mer, modcod):
    """D: MER above the decoding threshold of the MODCOD, dB (the firmware's $30 for one receiver)."""
    try:
        return round(float(mer) - MIN_MER_S2[modcod] / 10, 1)
    except (KeyError, ValueError, TypeError):
        return None


def capacity_kbps(srate, modcod, state):
    """Rough TS capacity of the channel from SR and MODCOD: DVB-S2 bits per symbol x code rate x
    about 0.95 framing (headers, pilots); DVB-S x 188/204 (Reed-Solomon)."""
    try:
        mod, rate = modcod.split()
        a, b = rate.split('/')
        bits = {'QPSK': 2, '8PSK': 3, '16APSK': 4, '32APSK': 5}[mod]
        eff = 188 / 204 if state == 'DVB-S' else 0.95
        return int(srate * bits * int(a) / int(b) * eff)
    except (ValueError, KeyError, AttributeError):
        return None


class StatusProtocol(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        if addr[0] != TUNER:
            return
        try:
            self.status(data)
        except (ValueError, KeyError, IndexError):
            pass

    def status(self, data):
        d = {}
        for line in data.decode(errors='replace').replace('\x00', '').split('\r\n'):
            if line.startswith('$') and ',' in line:
                k, v = line[1:].split(',', 1)
                d[k] = v
        rx = int(d.get('0', 0) or 0)
        if rx in S.status:
            now, prev = time.time(), S.status[rx]
            st = {'state': d.get('1', ''), 'mer': d.get('12', ''), 'name': d.get('13', ''),
                  'provider': d.get('14', ''), 'modcod': d.get('18', ''), 'freq': d.get('6', ''),
                  'srate': d.get('9', ''), 'video': d.get('31', ''), 'audio': d.get('34', ''),
                  'd': d.get('30', ''), 'null': d.get('15', ''), 'lock': d.get('38', ''),
                  'pkterr': d.get('39', ''), 'at': now}
            # measured TS bitrate from the tuner's received packet count ($91), over >= 4 s
            try:
                pk = int(d.get('91', ''))
            except ValueError:
                pk = None
            base = prev.get('base')
            if pk is None or base is None or pk < base[1]:
                st['base'], st['tsrate'] = (now, pk) if pk is not None else None, None
            elif now - base[0] >= 4:
                st['tsrate'] = int((pk - base[1]) * 188 * 8 / (now - base[0]) / 1000)
                st['base'] = (now, pk)
            else:
                st['base'], st['tsrate'] = base, prev.get('tsrate')
            S.status[rx] = st


# ---------------------------------------------------------------------------------------------
# ffmpeg: RX1 TS -> HLS

def clear_hls():
    HLS_DIR.mkdir(parents=True, exist_ok=True)
    for p in HLS_DIR.iterdir():
        if p.is_file():
            p.unlink()


FFMPEG_LOCK = asyncio.Lock()


async def start_ffmpeg():
    async with FFMPEG_LOCK:                              # api_tune and the supervisor could both start one
        await _stop_ffmpeg()
        await _start_ffmpeg()


async def stop_ffmpeg():
    async with FFMPEG_LOCK:
        await _stop_ffmpeg()


async def _start_ffmpeg():
    clear_hls()
    args = ['ffmpeg', '-hide_banner', '-loglevel', 'warning', '-nostdin',
            '-fflags', '+genpts+discardcorrupt', '-analyzeduration', '4M', '-probesize', '4M',
            '-i', f'udp://0.0.0.0:{BASE + 41}?overrun_nonfatal=1&fifo_size=500000&timeout=20000000',
            '-map', '0:v:0', '-map', '0:a:0?',
            '-vf', f"scale=-2:'min({HEIGHT},ih)'",
            '-fpsmax', '30',                             # 60 fps sources: better pictures at this bitrate
            '-c:v', 'libx264', '-preset', 'veryfast', '-profile:v', 'main',
            '-b:v', f'{KBPS}k', '-maxrate', f'{int(KBPS * 1.15)}k', '-bufsize', f'{KBPS * 2}k',
            '-force_key_frames', 'expr:gte(t,n_forced*2)',
            '-c:a', 'aac', '-b:a', '48k', '-ac', '1', '-ar', '48000',
            '-f', 'hls', '-hls_time', '2', '-hls_list_size', '6',
            '-hls_flags', 'delete_segments+omit_endlist+independent_segments',
            '-hls_segment_filename', str(HLS_DIR / 'seg_%05d.ts'), str(HLS_DIR / 'index.m3u8')]
    S.ffmpeg = await asyncio.create_subprocess_exec(*args)


async def _stop_ffmpeg():
    p = S.ffmpeg
    if p and p.returncode is None:
        p.terminate()
        try:
            await asyncio.wait_for(p.wait(), 5)
        except asyncio.TimeoutError:
            p.kill()
            await p.wait()
    S.ffmpeg = None


async def supervisor():
    """Resend commands, restart ffmpeg if it died, stop viewing when nobody watches."""
    last_send = 0.0
    while True:
        now = time.time()
        for cid, t in list(S.viewers.items()):
            if now - t > VIEWER_TTL_S:
                del S.viewers[cid]
        if S.viewers:
            S.last_viewer = now
        viewed_station(now)
        for f, st in list(S.stations.items()):
            if now - st['seen'] > STATION_TTL_S:
                del S.stations[f]
        if S.viewing and now - S.last_viewer > IDLE_STOP_S and now - S.viewing['since'] > IDLE_STOP_S:
            S.viewing = None
            await stop_ffmpeg()
            clear_hls()
            apply_mode()
            last_send = now
        if S.viewing and not FFMPEG_LOCK.locked() and (S.ffmpeg is None or S.ffmpeg.returncode is not None):
            await start_ffmpeg()                         # no TS for 20 s or a decoder error
        if now - last_send > RESEND_S:
            apply_mode()
            last_send = now
        await asyncio.sleep(2)


# ---------------------------------------------------------------------------------------------
# HTTP

CLIENT_GAP_S = 4 * CHANGE_GAP_S                         # one address: one change per minute by default
last_by_client = {}


def client_ip(request):
    fwd = request.headers.get('X-Forwarded-For', '')     # set by the nginx in front
    return (fwd.split(',')[0].strip() if fwd else request.remote) or '?'


def change_allowed(request, what):
    now, ip = time.time(), client_ip(request)
    wait = CHANGE_GAP_S - (now - S.last_change)
    if wait > 0:
        raise web.HTTPTooManyRequests(text=f'someone just changed the station; try again in {int(wait) + 1} s')
    wait = CLIENT_GAP_S - (now - last_by_client.get(ip, 0))
    if wait > 0:
        raise web.HTTPTooManyRequests(text=f'you changed the station a moment ago; try again in {int(wait) + 1} s')
    S.last_change = last_by_client[ip] = now
    for k in [k for k, t in last_by_client.items() if now - t > CLIENT_GAP_S]:
        del last_by_client[k]
    print(f'{time.strftime("%Y-%m-%d %H:%M:%S")} {ip} {what}', flush=True)


async def index(request):
    return web.FileResponse(STATIC / 'index.html', headers={'Cache-Control': 'no-cache'})


def dedupe(stations):
    """The demodulator also locks on a station from the neighbouring channel (500 kHz away, the
    carrier search reaches it), so one signal can show up twice: keep, of entries with the same
    callsign closer than their symbol rate, the one with the better MER."""
    def mer(s):
        try:
            return float(s.get('mer') or -99)
        except ValueError:
            return -99.0
    keep = []
    for st in stations:
        twin = next((k for k in keep if st.get('name') and k.get('name') == st['name']
                     and abs(k['freq'] - st['freq']) <= max(k['srate'], st['srate'])), None)
        if twin is None:
            keep.append(st)
        elif mer(st) > mer(twin):
            keep[keep.index(twin)] = st
    return keep


def viewed_station(now):
    """While someone watches, RX2 skips the station RX1 is locked on (firmware 0v3e-ota16), so the list
    entry for it is kept up to date from RX1's status instead."""
    v, st = S.viewing, S.status[1]
    if not v or now - st.get('at', 0) > 5 or st.get('state') not in ('DVB-S2', 'DVB-S'):
        return
    e = S.stations.get(v['freq'], {'first': now})
    e.update(freq=v['freq'], srate=v['srate'], state=st['state'], mer=st['mer'], modcod=st['modcod'],
             rx=1, seen=now, d=decode_margin(st['mer'], st['modcod']),
             capacity=capacity_kbps(v['srate'], st['modcod'], st['state']))
    for k in ('name', 'provider', 'video', 'audio'):
        if st.get(k):
            e[k] = st[k]
    e.setdefault('agc2', '')
    S.stations[v['freq']] = e


def rx1_view(now):
    st = S.status[1]
    if now - st.get('at', 0) >= 10:
        return {}
    out = {k: v for k, v in st.items() if k != 'base'}
    try:
        out['useful'] = int(st['tsrate'] * (1 - float(st['null']) / 100)) if st.get('tsrate') is not None else None
    except (ValueError, TypeError):
        out['useful'] = None
    return out


async def api_state(request):
    cid = request.query.get('id', '')[:40]
    if cid and request.query.get('watching') == '1':
        S.viewers[cid] = time.time()
    now = time.time()
    stations = dedupe(sorted(S.stations.values(), key=lambda s: s['freq']))
    return web.json_response({
        'now': now,
        'stations': [{**s, 'age': round(now - s['seen'], 1)} for s in stations],
        'viewing': S.viewing,
        'video': S.viewing is not None and (HLS_DIR / 'index.m3u8').exists(),
        'rx1': rx1_view(now),
        'passes': {k: {**v, 'age': round(now - v['at'], 1)} for k, v in S.passes.items() if v and now - v['at'] < 60},
        'viewers': len(S.viewers),
        'mode': S.mode,
    }, headers={'Cache-Control': 'no-store'})


async def api_tune(request):
    try:
        body = await request.json()
        freq, srate = int(body['freq']), int(body['srate'])
    except (ValueError, KeyError, TypeError, AttributeError):
        raise web.HTTPBadRequest(text='expected {"freq": kHz, "srate": kS/s}')
    if not (10489000 <= freq <= 10500000) or srate not in (33, 66, 125, 250, 333, 500, 1000, 1500, 2000):
        raise web.HTTPBadRequest(text='not a QO-100 WB channel')
    if S.viewing and S.viewing['freq'] == freq and S.viewing['srate'] == srate:
        S.last_viewer = time.time()
        return web.json_response({'ok': True})          # already on: just watch
    change_allowed(request, f'tune {freq} {srate}')
    S.viewing = {'freq': freq, 'srate': srate, 'since': time.time()}
    S.last_viewer = time.time()
    apply_mode()
    await start_ffmpeg()
    return web.json_response({'ok': True})


async def api_stop(request):
    if not S.viewing:
        return web.json_response({'ok': True})
    change_allowed(request, 'stop')
    S.viewing = None
    await stop_ffmpeg()
    clear_hls()
    apply_mode()
    return web.json_response({'ok': True})


async def hls(request):
    name = request.match_info['name']
    if '/' in name or not (name.endswith('.m3u8') or name.endswith('.ts')):
        raise web.HTTPNotFound()
    try:
        body = (HLS_DIR / name).read_bytes()
    except OSError:                                      # not there, or deleted by ffmpeg meanwhile
        raise web.HTTPNotFound()
    cache = 'no-cache' if name.endswith('.m3u8') else 'max-age=60'
    ctype = 'application/vnd.apple.mpegurl' if name.endswith('.m3u8') else 'video/mp2t'
    return web.Response(body=body, content_type=ctype, headers={'Cache-Control': cache})


async def on_startup(app):
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(ScanProtocol, local_addr=('0.0.0.0', BASE + 5))
    await loop.create_datagram_endpoint(StatusProtocol, local_addr=('0.0.0.0', BASE + 1))
    await loop.create_datagram_endpoint(StatusProtocol, local_addr=('0.0.0.0', BASE + 3))
    clear_hls()
    apply_mode()
    app['sup'] = asyncio.create_task(supervisor())


async def on_cleanup(app):
    app['sup'].cancel()
    await stop_ffmpeg()


@web.middleware
async def app_header(request, handler):
    resp = await handler(request)
    resp.headers['X-App'] = 'qo100web'
    return resp


async def static_file(request):
    name = request.match_info['name']
    p = STATIC / name
    if '/' in name or name.startswith('.') or not p.is_file():
        raise web.HTTPNotFound()
    return web.FileResponse(p, headers={'Cache-Control': 'no-cache'})


def main():
    if not shutil.which('ffmpeg'):
        raise SystemExit('ffmpeg not found')
    if not TUNER:
        raise SystemExit('set TUNER (the tuner IP address) in .env')
    app = web.Application(middlewares=[app_header])
    # every route also under any prefix, for proxies that pass /picotuner/... through unchanged
    for pre in ('', '/{prefix:.+}'):
        app.router.add_get(pre + '/', index)
        app.router.add_get(pre + '/api/state', api_state)
        app.router.add_post(pre + '/api/tune', api_tune)
        app.router.add_post(pre + '/api/stop', api_stop)
        app.router.add_get(pre + '/hls/{name}', hls)
        app.router.add_get(pre + '/static/{name}', static_file)
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    web.run_app(app, port=HTTP_PORT, access_log=None)


if __name__ == '__main__':
    main()
