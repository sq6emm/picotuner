"""QO-100 web monitor for a PicoTuner (PicoTuneWH 0v3e-ota13 or later).

Runs next to the tuner on the LAN, behind any reverse proxy and under any path (all URLs are
relative). It drives the tuner over UDP:
  - scanning: the in-firmware band scan (SCANF=/SCANSR=) on the QO-100 WB bandplan; with nobody
    watching both receivers share the plan, while someone watches RX2 scans it alone;
  - viewing: RX1 tuned to the chosen station, its TS (UDP base+41) re-encoded by ffmpeg to a small
    HLS stream (480p, ~400 kb/s by default); it stops IDLE_STOP_S after the last viewer leaves.
Anyone can watch and choose a station (OPEN_CONTROL=1, the default; one change per
CHANGE_GAP_S for everybody); with OPEN_CONTROL=0 choosing needs PASSWORD.

Environment: TUNER (IP), PASSWORD, HTTP_PORT (8090), BASE_PORT (9900), LNB_OFFSET (9750000),
VGX (HI), VIDEO_HEIGHT (480), VIDEO_KBPS (400), IDLE_STOP_S (60), HLS_DIR (/hls),
OPEN_CONTROL (1), CHANGE_GAP_S (15).
"""
import asyncio
import hashlib
import hmac
import json
import os
import secrets
import shutil
import socket
import time
from pathlib import Path

from aiohttp import web

TUNER = os.environ.get('TUNER', 'TUNER_IP')
PASSWORD = os.environ.get('PASSWORD', '')
HTTP_PORT = int(os.environ.get('HTTP_PORT', '8090'))
BASE = int(os.environ.get('BASE_PORT', '9900'))
OFFSET = int(os.environ.get('LNB_OFFSET', '9750000'))
VGX = os.environ.get('VGX', 'HI')
HEIGHT = int(os.environ.get('VIDEO_HEIGHT', '480'))
KBPS = int(os.environ.get('VIDEO_KBPS', '400'))
IDLE_STOP_S = int(os.environ.get('IDLE_STOP_S', '60'))
HLS_DIR = Path(os.environ.get('HLS_DIR', '/hls'))
OPEN_CONTROL = os.environ.get('OPEN_CONTROL', '1') == '1'
CHANGE_GAP_S = int(os.environ.get('CHANGE_GAP_S', '15'))
STATIC = Path(__file__).parent / 'static'

RESEND_S = 240            # the firmware stops sending off-net after 15 min; resend well before
STATION_TTL_S = 30        # a station not reported for this long is dropped from the list
VIEWER_TTL_S = 12         # a page that stopped polling is no longer a viewer
SECRET = secrets.token_bytes(32)

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
        for line in data.decode(errors='replace').replace('\x00', '').splitlines():
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
                st.setdefault('first', time.time())
                S.stations[freq] = st
            elif line.startswith('[from@wh:SCANPASS]'):
                rx = int(kv.get('rx', 0))
                S.passes[rx] = {'time': int(kv.get('time', 0)), 'found': int(kv.get('found', 0)),
                                'at': time.time()}


class StatusProtocol(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        d = {}
        for line in data.decode(errors='replace').replace('\x00', '').split('\r\n'):
            if line.startswith('$') and ',' in line:
                k, v = line[1:].split(',', 1)
                d[k] = v
        rx = int(d.get('0', 0) or 0)
        if rx in S.status:
            S.status[rx] = {'state': d.get('1', ''), 'mer': d.get('12', ''), 'name': d.get('13', ''),
                            'provider': d.get('14', ''), 'modcod': d.get('18', ''), 'freq': d.get('6', ''),
                            'srate': d.get('9', ''), 'video': d.get('31', ''), 'lock': d.get('38', ''),
                            'pkterr': d.get('39', ''), 'at': time.time()}


# ---------------------------------------------------------------------------------------------
# ffmpeg: RX1 TS -> HLS

def clear_hls():
    HLS_DIR.mkdir(parents=True, exist_ok=True)
    for p in HLS_DIR.iterdir():
        if p.is_file():
            p.unlink()


async def start_ffmpeg():
    await stop_ffmpeg()
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


async def stop_ffmpeg():
    p, S.ffmpeg = S.ffmpeg, None
    if p and p.returncode is None:
        p.terminate()
        try:
            await asyncio.wait_for(p.wait(), 5)
        except asyncio.TimeoutError:
            p.kill()


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
        for f, st in list(S.stations.items()):
            if now - st['seen'] > STATION_TTL_S:
                del S.stations[f]
        if S.viewing and now - S.last_viewer > IDLE_STOP_S and now - S.viewing['since'] > IDLE_STOP_S:
            S.viewing = None
            await stop_ffmpeg()
            clear_hls()
            apply_mode()
            last_send = now
        if S.viewing and (S.ffmpeg is None or S.ffmpeg.returncode is not None):
            await start_ffmpeg()                         # no TS for 20 s or a decoder error
        if now - last_send > RESEND_S:
            apply_mode()
            last_send = now
        await asyncio.sleep(2)


# ---------------------------------------------------------------------------------------------
# HTTP

def make_token():
    exp = str(int(time.time()) + 12 * 3600)
    return exp + '.' + hmac.new(SECRET, exp.encode(), hashlib.sha256).hexdigest()


def control_ok(request):
    return OPEN_CONTROL or token_ok(request)


def change_allowed():
    wait = CHANGE_GAP_S - (time.time() - S.last_change)
    if wait > 0:
        raise web.HTTPTooManyRequests(text=f'someone just changed the station; try again in {int(wait) + 1} s')
    S.last_change = time.time()


def token_ok(request):
    tok = request.headers.get('X-Token', '')
    if '.' not in tok:
        return False
    exp, sig = tok.split('.', 1)
    good = hmac.new(SECRET, exp.encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(sig, good) and exp.isdigit() and int(exp) > time.time()


async def index(request):
    return web.FileResponse(STATIC / 'index.html', headers={'Cache-Control': 'no-cache'})


async def api_state(request):
    cid = request.query.get('id', '')[:40]
    if cid and request.query.get('watching') == '1':
        S.viewers[cid] = time.time()
    now = time.time()
    stations = sorted(S.stations.values(), key=lambda s: s['freq'])
    return web.json_response({
        'now': now,
        'stations': [{**s, 'age': round(now - s['seen'], 1)} for s in stations],
        'viewing': S.viewing,
        'video': S.viewing is not None and (HLS_DIR / 'index.m3u8').exists(),
        'rx1': S.status[1] if now - S.status[1].get('at', 0) < 10 else {},
        'passes': {k: v for k, v in S.passes.items() if v and now - v['at'] < 60},
        'viewers': len(S.viewers),
        'mode': S.mode,
        'auth_ok': control_ok(request),
        'open': OPEN_CONTROL,
    }, headers={'Cache-Control': 'no-store'})


async def api_login(request):
    body = await request.json()
    if not PASSWORD or not hmac.compare_digest(str(body.get('password', '')), PASSWORD):
        await asyncio.sleep(1.0)                         # slow down guessing
        raise web.HTTPForbidden(text='bad password')
    return web.json_response({'token': make_token()})


async def api_tune(request):
    if not control_ok(request):
        raise web.HTTPForbidden(text='login first')
    body = await request.json()
    freq, srate = int(body['freq']), int(body['srate'])
    if not (10489000 <= freq <= 10500000) or srate not in (33, 66, 125, 250, 333, 500, 1000, 1500, 2000):
        raise web.HTTPBadRequest(text='not a QO-100 WB channel')
    if S.viewing and S.viewing['freq'] == freq and S.viewing['srate'] == srate:
        S.last_viewer = time.time()
        return web.json_response({'ok': True})          # already on: just watch
    change_allowed()
    S.viewing = {'freq': freq, 'srate': srate, 'since': time.time()}
    S.last_viewer = time.time()
    apply_mode()
    await start_ffmpeg()
    return web.json_response({'ok': True})


async def api_stop(request):
    if not control_ok(request):
        raise web.HTTPForbidden(text='login first')
    change_allowed()
    S.viewing = None
    await stop_ffmpeg()
    clear_hls()
    apply_mode()
    return web.json_response({'ok': True})


async def hls(request):
    name = request.match_info['name']
    if '/' in name or not (name.endswith('.m3u8') or name.endswith('.ts')):
        raise web.HTTPNotFound()
    p = HLS_DIR / name
    if not p.exists():
        raise web.HTTPNotFound()
    cache = 'no-cache' if name.endswith('.m3u8') else 'max-age=60'
    ctype = 'application/vnd.apple.mpegurl' if name.endswith('.m3u8') else 'video/mp2t'
    return web.Response(body=p.read_bytes(), content_type=ctype, headers={'Cache-Control': cache})


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
    app = web.Application(middlewares=[app_header])
    # every route also under any prefix, for proxies that pass /picotuner/... through unchanged
    for pre in ('', '/{prefix:.+}'):
        app.router.add_get(pre + '/', index)
        app.router.add_get(pre + '/api/state', api_state)
        app.router.add_post(pre + '/api/login', api_login)
        app.router.add_post(pre + '/api/tune', api_tune)
        app.router.add_post(pre + '/api/stop', api_stop)
        app.router.add_get(pre + '/hls/{name}', hls)
        app.router.add_get(pre + '/static/{name}', static_file)
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    web.run_app(app, port=HTTP_PORT, access_log=None)


if __name__ == '__main__':
    main()
