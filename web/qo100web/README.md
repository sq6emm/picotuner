# QO-100 WB web monitor

A small web page for a PicoTuner (PicoTuneWH 0v3e-ota13 or later): the tuner's own band scan of
the QO-100 wideband transponder as a live list, and the chosen station as a re-encoded,
low-bitrate HLS stream (480p, about 400 kb/s) for watching over the internet.

- With nobody watching, both receivers scan the BATC WB bandplan (about 2 s per pass).
- Anyone picks a station (one change per 15 s for everybody; `OPEN_CONTROL=0` makes choosing
  need `PASSWORD`): RX1 tunes to it, ffmpeg re-encodes its TS to HLS, and RX2 keeps scanning the
  whole plan. Everybody watches the same station.
- When nobody has watched for `IDLE_STOP_S` (60 s), the stream stops and both receivers scan
  again.

The app talks to the tuner over UDP from the machine it runs on, so the tuner sends everything
there. Tuning the same tuner from somewhere else (VLC, scripts) redirects that receiver's status
and TS away from the app until its next command (it resends every 4 minutes).

## Run

```sh
cp .env.example .env      # TUNER if it is not TUNER_IP; PASSWORD only with OPEN_CONTROL=0
docker compose up -d --build
```

The page is on `http://<host>:8090/`.

## Behind nginx under a path

Every URL in the page is relative, so any prefix works as long as the proxy strips it and the
page is opened with the trailing slash:

```nginx
location = /qo100 { return 301 /qo100/; }
location /qo100/ {
    proxy_pass http://APP_HOST:8090/;   # trailing slash: strips /qo100/
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_buffering off;                       # live HLS playlist
}
```

## API

- `GET api/state?id=<client>&watching=0|1`: stations, RX1 status, what is on, scan pass times.
- `POST api/login {"password": ...}` returns a token; send it as `X-Token`.
- `POST api/tune {"freq": kHz, "srate": kS/s}` and `POST api/stop` (token needed only with
  `OPEN_CONTROL=0`; 429 within `CHANGE_GAP_S` of the last change).
- `GET hls/index.m3u8` and its segments.
