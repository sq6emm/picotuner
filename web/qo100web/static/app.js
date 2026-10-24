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

function dClass(d) {
  // colour by D, the margin above the MODCOD's decoding threshold
  const v = parseFloat(d);
  if (isNaN(v)) return '';
  return v >= 2 ? 'mer-good' : v >= 0.5 ? 'mer-mid' : 'mer-low';
}
function dText(d) { const v = parseFloat(d); return isNaN(v) ? '' : (v > 0 ? '+' : '') + v.toFixed(1); }
function codecs(st) { return [st.video, st.audio].filter(Boolean).join(' / '); }
function kbps(v) { return v == null ? '' : v >= 1000 ? (v / 1000).toFixed(2) + ' Mb/s' : v + ' kb/s'; }

function render(s) {
  last = s;
  const p = Object.entries(s.passes || {}).map(([rx, v]) => `RX${rx} ${(v.time / 1000).toFixed(1)} s`).join(', ');
  $('scaninfo').textContent = p ? `scan pass: ${p}` : '';

  const rows = s.stations.map((st) => {
    const on = s.viewing && s.viewing.freq === st.freq;
    const btn = `<button class="ghost" data-f="${st.freq}" data-s="${st.srate}">${on ? 'On' : 'Watch'}</button>`;
    return `<tr class="${on ? 'on' : ''}"><td>${(st.freq / 1000).toFixed(3)}</td><td>${st.srate}</td>` +
      `<td class="call" title="${escapeHtml(st.name || '')}">${escapeHtml(st.name || '')}</td>` +
      `<td class="mer ${dClass(st.d)}">${st.mer || ''}</td><td class="${dClass(st.d)}">${dText(st.d)}</td>` +
      `<td>${escapeHtml(st.modcod || '')}</td><td>${escapeHtml(codecs(st))}</td>` +
      `<td title="estimated channel capacity">${st.capacity ? '≈' + kbps(st.capacity) : ''}</td>` +
      `<td>${Math.round(st.age)} s</td><td>${btn}</td></tr>`;
  });
  $('stations').innerHTML = rows.length ? rows.join('') : '<tr><td colspan="10" class="muted">No stations found yet…</td></tr>';

  const rx1 = s.rx1 || {};
  if (s.viewing) {
    $('nowname').textContent = rx1.name || 'tuning…';
    $('nowfreq').textContent = `${(s.viewing.freq / 1000).toFixed(3)} MHz · ${s.viewing.srate} kS/s`;
    const d = rx1.d !== '' && rx1.d != null ? rx1.d : null;
    $('nowinfo').innerHTML = [
      escapeHtml(rx1.state || ''),
      rx1.mer ? `<b class="${dClass(d)}">MER ${escapeHtml(rx1.mer)} dB</b>` : '',
      d != null ? `<span class="${dClass(d)}">D ${dText(d)}</span>` : '',
      escapeHtml(rx1.modcod || ''),
      escapeHtml([rx1.video, rx1.audio].filter(Boolean).join(' / ')),
      rx1.tsrate != null ? `TS ${kbps(rx1.tsrate)}` + (rx1.useful != null ? ` (${kbps(rx1.useful)} without null packets)` : '') : '',
    ].filter(Boolean).join(' · ');
  } else {
    $('nowname').textContent = '—';
    $('nowfreq').textContent = '';
    $('nowinfo').textContent = 'Both receivers are scanning.';
  }
  $('viewers').textContent = s.viewers ? `${s.viewers} watching` : '';
  $('stop').hidden = !s.viewing;
  $('watch').hidden = !(s.video && !watching);
  $('novideo').hidden = Boolean(s.video && watching);
  $('novideotext').textContent = watching && s.viewing ? 'Tuning… starting the stream'
    : s.viewing && s.video ? 'On air — press Watch' : 'Nothing on air from this receiver.';
  drawSpectrum(s);
  if (watching && !s.viewing) stopPlayer();                // the stream was stopped
  else if (watching && !s.video && playingFreq) stopPlayer(true);   // new station: wait for its stream
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
  const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = 260;
  cv.width = W * dpr; cv.height = H * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const pad = { l: 34, r: 8, t: 70, b: 22 }, pw = W - pad.l - pad.r, ph = H - pad.t - pad.b;
  // decoded stations only; height = MER, which for a locked DVB-S2 signal is its C/N above the noise
  const pts = (s.stations || []).map((st) => {
    const m = parseFloat(st.mer);
    return { f: st.freq, sr: st.srate, db: isNaN(m) ? 0.5 : Math.max(0.5, m) };
  });
  const top = 15;                                   // fixed scale
  const x = (f) => pad.l + (f - F0) / (F1 - F0) * pw, y = (db) => pad.t + ph - Math.max(0, db) / top * ph;
  ctx.clearRect(0, 0, W, H);
  ctx.font = '11px system-ui'; ctx.lineWidth = 1;
  ctx.strokeStyle = cssVar('--line'); ctx.fillStyle = cssVar('--muted');
  for (let f = F0; f <= F1; f += 500) {           // grid every 0.5 MHz, labels every 1 MHz
    ctx.beginPath(); ctx.moveTo(x(f) + .5, pad.t); ctx.lineTo(x(f) + .5, pad.t + ph); ctx.stroke();
    if ((f - 10490000) % 1000 === 0) ctx.fillText((f / 1000).toFixed(0), x(f) - 16, H - 6);
  }
  for (let d = 0; d <= top; d += 5) {
    ctx.beginPath(); ctx.moveTo(pad.l, y(d) + .5); ctx.lineTo(pad.l + pw, y(d) + .5); ctx.stroke();
    ctx.fillText(`${d}`, 8, y(d) + 4);
  }
  ctx.fillText('MER dB', 4, pad.t - 6);
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
    const near = pts.find((p) => p.f === st.freq && p.sr === st.srate);
    const lvl = Math.min(top, near ? near.db : 0.5);
    const on = s.viewing && s.viewing.freq === st.freq;
    const cx = x(st.freq), cy = y(lvl) - 6;
    ctx.fillStyle = on ? cssVar('--good') : cssVar('--fg');
    ctx.textAlign = 'center';
    ctx.font = (on ? 'bold ' : '') + '12px system-ui';
    ctx.fillText(st.name || `${(st.freq / 1000).toFixed(2)}`, cx, Math.max(12, cy - 26));
    const dc = dClass(st.d);
    ctx.font = 'bold 13px system-ui';
    ctx.fillStyle = dc === 'mer-good' ? cssVar('--good') : dc === 'mer-mid' ? cssVar('--warn') : dc === 'mer-low' ? cssVar('--bad') : cssVar('--fg');
    ctx.fillText(`MER ${st.mer || '?'}` + (st.d != null ? `  D ${dText(st.d)}` : ''), cx, Math.max(26, cy - 12));
    ctx.font = '11px system-ui'; ctx.fillStyle = cssVar('--muted');
    ctx.fillText([`${st.srate} kS`, codecs(st)].filter(Boolean).join(' · '), cx, Math.max(38, cy));
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
