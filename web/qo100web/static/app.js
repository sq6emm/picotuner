// QO-100 WB Monitor front end. All URLs are relative, so it works under any reverse-proxy path.
'use strict';
const $ = (id) => document.getElementById(id);
const clientId = Math.random().toString(36).slice(2, 12);
let token = '';
try { token = localStorage.getItem('qo100token') || ''; } catch (e) { /* storage blocked */ }
let watching = false, hls = null, last = null, playingFreq = null;

function headers() {
  return { 'Content-Type': 'application/json', 'X-Token': token };
}

async function post(url, body) {
  const r = await fetch(url, { method: 'POST', headers: headers(), body: JSON.stringify(body || {}) });
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
  const loggedIn = s.auth_ok;
  $('login').hidden = s.open || loggedIn;
  $('logged').hidden = s.open || !loggedIn;
  const p = Object.entries(s.passes || {}).map(([rx, v]) => `RX${rx} ${(v.time / 1000).toFixed(1)} s`).join(', ');
  $('scaninfo').textContent = p ? `scan pass: ${p}` : '';

  const rows = s.stations.map((st) => {
    const on = s.viewing && s.viewing.freq === st.freq;
    const btn = loggedIn ? `<button class="ghost" data-f="${st.freq}" data-s="${st.srate}">${on ? 'On' : 'Watch'}</button>` : '';
    return `<tr class="${on ? 'on' : ''}"><td>${(st.freq / 1000).toFixed(3)}</td><td>${st.srate}</td>` +
      `<td>${escapeHtml(st.name || '')}</td><td class="${merClass(st.mer)}">${st.mer || ''}</td>` +
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
  $('stop').hidden = !(loggedIn && s.viewing);
  $('watch').hidden = !(s.video && !watching);
  $('novideo').hidden = Boolean(s.video && watching);
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

async function poll() {
  try {
    const r = await fetch(`api/state?id=${clientId}&watching=${watching ? 1 : 0}`, { headers: { 'X-Token': token } });
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

$('login').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  try {
    const r = await post('api/login', { password: $('pw').value });
    token = r.token;
    try { localStorage.setItem('qo100token', token); } catch (e) { /* ignore */ }
    $('pw').value = '';
  } catch (e) { alert('Wrong password'); }
});
$('logout').addEventListener('click', (ev) => {
  ev.preventDefault();
  token = '';
  try { localStorage.removeItem('qo100token'); } catch (e) { /* ignore */ }
});

poll();
