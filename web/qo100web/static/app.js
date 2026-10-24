// QO-100 WB Monitor front end. All URLs are relative, so it works under any reverse-proxy path.
'use strict';
const $ = (id) => document.getElementById(id);
const clientId = Math.random().toString(36).slice(2, 12);
let watching = false, hls = null, last = null, playingFreq = null;


async function post(url, body) {
  const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}

function merClass(m) {
  const v = parseFloat(m);
  if (isNaN(v)) return '';
  return v >= 6 ? 'mer-good' : v >= 3 ? 'mer-mid' : 'mer-low';
}

function level(agc2, srate) {
  // AGC2 is the in-channel gain: lower = stronger. Rough bar against the empty-channel level.
  const a = parseInt(agc2, 10);
  if (isNaN(a)) return '';
  const noise = srate >= 1000 ? 590 : 220;
  const pct = Math.max(5, Math.min(100, Math.round(100 * (1 - a / noise) * 1.6)));
  return `<span class="bar" title="AGC2 ${a}"><i style="width:${pct}%"></i></span>`;
}

function render(s) {
  last = s;
  const p = Object.entries(s.passes || {}).map(([rx, v]) => `RX${rx} ${(v.time / 1000).toFixed(1)} s`).join(', ');
  $('scaninfo').textContent = p ? `scan pass: ${p}` : '';

  const rows = s.stations.map((st) => {
    const on = s.viewing && s.viewing.freq === st.freq;
    const btn = `<button class="ghost" data-f="${st.freq}" data-s="${st.srate}">${on ? 'On' : 'Watch'}</button>`;
    return `<tr class="${on ? 'on' : ''}"><td>${(st.freq / 1000).toFixed(3)}</td><td>${st.srate}</td>` +
      `<td class="call" title="${escapeHtml(st.name || '')}">${escapeHtml(st.name || '')}</td><td class="${merClass(st.mer)}">${st.mer || ''}</td>` +
      `<td>${escapeHtml(st.modcod || '')}</td><td>${level(st.agc2, st.srate)}</td><td>${Math.round(st.age)} s</td><td>${btn}</td></tr>`;
  });
  $('stations').innerHTML = rows.length ? rows.join('') : '<tr><td colspan="8" class="muted">No stations found yet…</td></tr>';

  const rx1 = s.rx1 || {};
  if (s.viewing) {
    $('nowname').textContent = rx1.name || 'tuning…';
    $('nowfreq').textContent = `${(s.viewing.freq / 1000).toFixed(3)} MHz · ${s.viewing.srate} kS/s`;
    $('nowinfo').textContent = [rx1.state, rx1.mer ? `MER ${rx1.mer} dB` : '', rx1.modcod].filter(Boolean).join(' · ');
  } else {
    $('nowname').textContent = '—';
    $('nowfreq').textContent = '';
    $('nowinfo').textContent = 'Both receivers are scanning.';
  }
  $('viewers').textContent = s.viewers ? `${s.viewers} watching` : '';
  $('stop').hidden = !s.viewing;
  $('watch').hidden = !(s.video && !watching);
  $('novideo').hidden = Boolean(s.video && watching);
  drawSpectrum(s);
  if (watching && !s.video) stopPlayer();
  if (watching && s.video && s.viewing && playingFreq !== s.viewing.freq) startPlayer(s.viewing.freq);
}

function escapeHtml(t) {
  return t.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function startPlayer(freq) {
  stopPlayer(true);
  watching = true;
  playingFreq = freq;
  const v = $('video');
  const src = 'hls/index.m3u8';
  if (window.Hls && Hls.isSupported()) {
    hls = new Hls({ liveSyncDurationCount: 3, manifestLoadingMaxRetry: 20, levelLoadingMaxRetry: 20 });
    hls.loadSource(src);
    hls.attachMedia(v);
    hls.on(Hls.Events.ERROR, (e, d) => { if (d.fatal) { hls.destroy(); hls = null; playingFreq = null; } });
  } else if (v.canPlayType('application/vnd.apple.mpegurl')) {
    v.src = src;
  }
  v.play().catch(() => {});
  $('novideo').hidden = true;
}

function stopPlayer(keepWatching) {
  if (hls) { hls.destroy(); hls = null; }
  const v = $('video');
  v.removeAttribute('src');
  v.load();
  playingFreq = null;
  if (!keepWatching) watching = false;
}

// ---- spectrum (relative levels from the scan) ----
const F0 = 10490500, F1 = 10499500;        // kHz shown
let specHits = [];
function cssVar(n) { return getComputedStyle(document.documentElement).getPropertyValue(n).trim(); }

function drawSpectrum(s) {
  const cv = $('spec'), ctx = cv.getContext('2d');
  const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = 220;
  cv.width = W * dpr; cv.height = H * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const pad = { l: 34, r: 8, t: 44, b: 22 }, pw = W - pad.l - pad.r, ph = H - pad.t - pad.b;
  // only decoded stations: the level of their own channel and symbol rate from the last scan
  const all = s.spectrum || [];
  const pts = (s.stations || []).map((st) => {
    const p = all.find((q) => q.f === st.freq && q.sr === st.srate);
    return { f: st.freq, sr: st.srate, db: p ? Math.max(1, p.db) : 1 };
  });
  const top = Math.max(12, Math.ceil(Math.max(0, ...pts.map((p) => p.db)) / 2) * 2 + 2);
  const x = (f) => pad.l + (f - F0) / (F1 - F0) * pw, y = (db) => pad.t + ph - Math.max(0, db) / top * ph;
  ctx.clearRect(0, 0, W, H);
  ctx.font = '11px system-ui'; ctx.lineWidth = 1;
  ctx.strokeStyle = cssVar('--line'); ctx.fillStyle = cssVar('--muted');
  for (let f = F0; f <= F1; f += 500) {           // grid every 0.5 MHz, labels every 1 MHz
    ctx.beginPath(); ctx.moveTo(x(f) + .5, pad.t); ctx.lineTo(x(f) + .5, pad.t + ph); ctx.stroke();
    if ((f - 10490000) % 1000 === 0) ctx.fillText((f / 1000).toFixed(0), x(f) - 16, H - 6);
  }
  for (let d = 0; d <= top; d += 4) {
    ctx.beginPath(); ctx.moveTo(pad.l, y(d) + .5); ctx.lineTo(pad.l + pw, y(d) + .5); ctx.stroke();
    ctx.fillText(`${d}`, 8, y(d) + 4);
  }
  ctx.fillText('dB', 8, pad.t - 6);
  // sweep time per receiver: full pass, of which the AGC2 screen, and how old the last pass is
  const sw = Object.entries(s.passes || {}).map(([rx, v]) =>
    `RX${rx} sweep ${(v.time / 1000).toFixed(1)} s (screen ${(v.screen / 1000).toFixed(1)} s, ${v.found} found) · ${Math.round(v.age)} s ago`);
  ctx.textAlign = 'end';
  sw.forEach((t, i) => ctx.fillText(t, W - pad.r, 12 + i * 13));
  ctx.textAlign = 'start';
  // wide symbol rates first, narrow ones on top
  const accent = cssVar('--accent');
  [...pts].sort((a, b) => b.sr - a.sr).forEach((p) => {
    const bw = p.sr * 1.35, x0 = x(p.f - bw / 2), x1 = x(p.f + bw / 2);
    ctx.globalAlpha = p.sr >= 1000 ? 0.35 : 0.75;
    ctx.fillStyle = accent;
    ctx.fillRect(x0, y(p.db), Math.max(1, x1 - x0 - 1), pad.t + ph - y(p.db));
  });
  ctx.globalAlpha = 1;
  // station labels like the BATC monitor: callsign and MER above the signal
  specHits = [];
  (s.stations || []).forEach((st) => {
    const near = pts.filter((p) => Math.abs(p.f - st.freq) <= 10 && p.sr === st.srate)[0];
    const lvl = near ? near.db : 2;
    const on = s.viewing && s.viewing.freq === st.freq;
    const cx = x(st.freq), cy = y(lvl) - 6;
    ctx.fillStyle = on ? cssVar('--good') : cssVar('--fg');
    ctx.textAlign = 'center';
    ctx.font = (on ? 'bold ' : '') + '12px system-ui';
    ctx.fillText(st.name || `${(st.freq / 1000).toFixed(2)}`, cx, Math.max(12, cy - 12));
    ctx.font = '11px system-ui'; ctx.fillStyle = cssVar('--muted');
    ctx.fillText(`${st.mer || '?'} dB · ${st.srate}`, cx, Math.max(24, cy));
    ctx.textAlign = 'start';
    specHits.push({ x0: x(st.freq - st.srate * 0.7), x1: x(st.freq + st.srate * 0.7), st });
  });
}

$('spec').addEventListener('click', async (ev) => {
  const r = ev.target.getBoundingClientRect(), px = ev.clientX - r.left;
  const hit = specHits.find((h) => px >= h.x0 && px <= h.x1);
  if (!hit) return;
  try {
    await post('api/tune', { freq: hit.st.freq, srate: hit.st.srate });
    watching = true; playingFreq = null;
  } catch (e) { alert(e.message); }
});

async function poll() {
  try {
    const r = await fetch(`api/state?id=${clientId}&watching=${watching ? 1 : 0}`);
    if (r.ok) render(await r.json());
  } catch (e) { /* retry next time */ }
  setTimeout(poll, 2000);
}

$('stations').addEventListener('click', async (ev) => {
  const b = ev.target.closest('button[data-f]');
  if (!b) return;
  b.disabled = true;
  try {
    await post('api/tune', { freq: +b.dataset.f, srate: +b.dataset.s });
    watching = true;
    playingFreq = null;                 // the next poll starts the player once the stream is up
  } catch (e) { alert(e.message); }
  b.disabled = false;
});

$('watch').addEventListener('click', () => { if (last && last.viewing) startPlayer(last.viewing.freq); });
$('stop').addEventListener('click', async () => { try { await post('api/stop'); } catch (e) { alert(e.message); } stopPlayer(); });

poll();
