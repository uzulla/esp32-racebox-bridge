'use strict';
// Logger Viewer。地図は Leaflet + 無料タイル(OpenStreetMap / 国土地理院)。

const $ = (id) => document.getElementById(id);

// ---- 地図 ----
const map = L.map('map', { zoomControl: true, preferCanvas: true }).setView([35.68, 139.76], 5);
const baseLayers = {
  'OpenStreetMap': L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 19, attribution: '© OpenStreetMap contributors' }),
  '地理院 標準地図': L.tileLayer('https://cyberjapandata.gsi.go.jp/xyz/std/{z}/{x}/{y}.png', { maxZoom: 18, attribution: '<a href="https://maps.gsi.go.jp/development/ichiran.html">地理院タイル</a>' }),
  '地理院 空中写真': L.tileLayer('https://cyberjapandata.gsi.go.jp/xyz/seamlessphoto/{z}/{x}/{y}.jpg', { maxZoom: 18, attribution: '<a href="https://maps.gsi.go.jp/development/ichiran.html">地理院タイル</a>' }),
};
baseLayers['OpenStreetMap'].addTo(map);
L.control.layers(baseLayers, null, { position: 'topright' }).addTo(map);
let trackLayer = L.layerGroup().addTo(map);
let marker = null;

// ---- データ ----
let D = null; // {n, t, speed, ..., posIdx, duration}
const COLS = ['t_ms', 'utc', 'fix', 'sats', 'lat', 'lon', 'alt_m', 'speed_kmh', 'heading_deg', 'hacc_m', 'acc_x', 'acc_y', 'acc_z', 'gyro_x', 'gyro_y', 'gyro_z', 'batt', 'charging'];

function parseCsv(text) {
  const lines = text.split(/\r?\n/);
  let header = null;
  const rows = [];
  for (const line of lines) {
    if (!line || line[0] === '#') continue;
    const f = line.split(',');
    if (!header) { header = f; continue; }
    if (f.length < header.length) continue; // 書きかけの最終行
    const o = {};
    header.forEach((h, i) => { o[h] = f[i]; });
    rows.push(o);
  }
  if (!header || !header.includes('t_ms')) throw new Error('ロガーのログ CSV ではありません');
  return rows;
}

function build(rows, name) {
  // fix は 0 / 2 / 3 だけが本物。起動直後のダミー行(fix=87 など)は捨てる
  const ok = rows.filter((r) => ['0', '2', '3'].includes(r.fix) && isFinite(+r.t_ms));
  const n = ok.length;
  if (!n) throw new Error('有効な行がありません');
  const num = (k) => Float64Array.from(ok, (r) => +r[k]);
  const d = { name, n, utc: ok.map((r) => r.utc) };
  for (const k of COLS.filter((c) => c !== 'utc')) d[k] = num(k);
  const t0 = d.t_ms[0];
  d.t = Float64Array.from(d.t_ms, (v) => (v - t0) / 1000);
  d.duration = d.t[n - 1];
  d.valid = new Uint8Array(n); // 位置が使える行
  d.posIdx = new Int32Array(n); // その行以前で最後に位置が使えた行
  let last = -1, nvalid = 0;
  for (let i = 0; i < n; i++) {
    if (d.fix[i] === 3 && d.hacc_m[i] < 100 && (d.lat[i] !== 0 || d.lon[i] !== 0)) { d.valid[i] = 1; last = i; nvalid++; }
    d.posIdx[i] = last;
  }
  d.nvalid = nvalid;
  // 静止時の加速度を基準にして重力を引く(車載の向きに依らず「動的 G」が出る)
  const ref = [0, 0, 0];
  let cnt = 0;
  for (let i = 0; i < n && cnt < 250; i++) {
    if (d.valid[i] && d.speed_kmh[i] < 1) { ref[0] += d.acc_x[i]; ref[1] += d.acc_y[i]; ref[2] += d.acc_z[i]; cnt++; }
  }
  if (!cnt) { for (let i = 0; i < Math.min(n, 250); i++) { ref[0] += d.acc_x[i]; ref[1] += d.acc_y[i]; ref[2] += d.acc_z[i]; cnt++; } }
  ref.forEach((_, k) => { ref[k] /= cnt; });
  d.dynG = Float64Array.from({ length: n }, (_, i) => Math.hypot(d.acc_x[i] - ref[0], d.acc_y[i] - ref[1], d.acc_z[i] - ref[2]));
  d.vmax = 0;
  for (let i = 0; i < n; i++) if (d.valid[i]) d.vmax = Math.max(d.vmax, d.speed_kmh[i]);
  d.gmax = Math.max(0.5, ...d.dynG);
  return d;
}

// ---- 表示 ----
const RAMP = [[59, 107, 255], [50, 230, 120], [255, 208, 0], [255, 70, 70]];
function speedColor(v, vmax) {
  const f = Math.min(1, v / Math.max(vmax, 1)) * (RAMP.length - 1);
  const i = Math.min(RAMP.length - 2, Math.floor(f)), u = f - i;
  const c = RAMP[i].map((a, k) => Math.round(a + (RAMP[i + 1][k] - a) * u));
  return `rgb(${c[0]},${c[1]},${c[2]})`;
}

function drawTrack() {
  trackLayer.clearLayers();
  if (marker) { map.removeLayer(marker); marker = null; }
  if (!D.nvalid) { $('notice').textContent = '位置(3D FIX)が取れていないログです。地図は表示できません'; return; }
  const renderer = L.canvas({ padding: 0.5 });
  const pts = [];
  for (let i = 0; i < D.n; i++) if (D.valid[i]) pts.push(i);
  let prev = pts[0];
  for (let k = 1; k < pts.length; k++) {
    const i = pts[k];
    const dt = D.t[i] - D.t[prev];
    if (dt < 0.2 && k < pts.length - 1) continue; // 5Hz 程度に間引く
    L.polyline([[D.lat[prev], D.lon[prev]], [D.lat[i], D.lon[i]]], { color: speedColor(D.speed_kmh[i], D.vmax), weight: 5, opacity: 0.9, renderer }).addTo(trackLayer);
    prev = i;
  }
  L.circleMarker([D.lat[pts[0]], D.lon[pts[0]]], { radius: 6, color: '#fff', fillColor: '#32e678', fillOpacity: 1, weight: 2 }).bindTooltip('START').addTo(trackLayer);
  const e = pts[pts.length - 1];
  L.circleMarker([D.lat[e], D.lon[e]], { radius: 6, color: '#fff', fillColor: '#ff4646', fillOpacity: 1, weight: 2 }).bindTooltip('END').addTo(trackLayer);
  const icon = L.divIcon({
    className: '', iconSize: [28, 28], iconAnchor: [14, 14],
    html: '<svg class="arrow" viewBox="-14 -14 28 28"><path d="M0-11 L8 9 L0 5 L-8 9 Z" fill="#00cdff" stroke="#fff" stroke-width="2" stroke-linejoin="round"/></svg>',
  });
  marker = L.marker([D.lat[pts[0]], D.lon[pts[0]]], { icon, interactive: false, keyboard: false, zIndexOffset: 1000 }).addTo(map);
  fitTrack();
}

function fitTrack() {
  const ll = [];
  for (let i = 0; i < D.n; i += 5) if (D.valid[i]) ll.push([D.lat[i], D.lon[i]]);
  if (ll.length) map.fitBounds(L.latLngBounds(ll), { padding: [40, 40], maxZoom: 18 });
}

const fmtT = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
function jst(utc) {
  const m = /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)/.exec(utc);
  if (!m || +m[1] < 2020) return null;
  const t = new Date(Date.UTC(+m[1], m[2] - 1, +m[3], +m[4], +m[5], +m[6]) + 9 * 3600e3);
  return t.toISOString().slice(0, 19).replace('T', ' ') + ' JST';
}

function upperBound(arr, x) { let lo = 0, hi = arr.length; while (lo < hi) { const m = (lo + hi) >> 1; if (arr[m] <= x) lo = m + 1; else hi = m; } return lo; }

let heading = 0;
function show(T) {
  if (!D) return;
  const i = Math.max(0, upperBound(D.t, T) - 1);
  const pi = D.posIdx[i];
  const fix3 = D.fix[i] === 3;
  $('speed').firstChild.nodeValue = fix3 ? Math.round(D.speed_kmh[i]) : '--';
  $('clock').textContent = jst(D.utc[i]) || '--';
  $('g').textContent = D.dynG[i].toFixed(2) + ' G';
  $('gbar').style.width = Math.min(100, D.dynG[i] / Math.max(1.5, D.gmax) * 100) + '%';
  $('acc').textContent = `${D.acc_x[i].toFixed(2)} / ${D.acc_y[i].toFixed(2)} / ${D.acc_z[i].toFixed(2)}`;
  $('alt').textContent = fix3 ? D.alt_m[i].toFixed(0) + ' m' : '--';
  $('gps').textContent = fix3 ? `3D FIX ${D.sats[i]} 衛星` : D.fix[i] === 2 ? `2D FIX ${D.sats[i]} 衛星` : 'FIX なし';
  $('hacc').textContent = fix3 ? D.hacc_m[i].toFixed(1) + ' m' : '--';
  $('yaw').textContent = D.gyro_z[i].toFixed(1) + ' °/s';
  $('batt').textContent = D.batt[i] + (D.charging[i] ? ' % 充電中' : ' %');
  if (fix3 && D.speed_kmh[i] > 2) heading = D.heading_deg[i];
  $('hdg').textContent = fix3 ? `${Math.round(heading)}°` : '--';
  $('pos').textContent = `${fmtT(T)} / ${fmtT(D.duration)}`;
  $('seek').value = T;
  if (marker && pi >= 0) {
    const ll = [D.lat[pi], D.lon[pi]];
    marker.setLatLng(ll);
    const el = marker.getElement()?.firstChild;
    if (el) el.style.transform = `rotate(${heading}deg)`;
    if ($('follow').checked && !map.getBounds().pad(-0.3).contains(ll)) map.panTo(ll, { animate: false });
  }
  drawCursor(T);
}

// ---- グラフ(速度 + G。クリック / ドラッグでシーク) ----
const cv = $('chart'), ctx = cv.getContext('2d');
let base = null; // 背景(データ部分)のキャッシュ
function layoutChart() {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  cv.width = w * dpr; cv.height = h * dpr;
  base = document.createElement('canvas'); base.width = cv.width; base.height = cv.height;
  const c = base.getContext('2d');
  c.scale(dpr, dpr);
  if (!D) return;
  const padL = 40, padR = 8, mid = h / 2;
  const plot = (arr, y0, y1, max, color, label, unit) => {
    c.strokeStyle = '#26304d'; c.fillStyle = '#8591b0'; c.font = '11px sans-serif'; c.lineWidth = 1;
    c.beginPath(); c.moveTo(padL, y0); c.lineTo(w - padR, y0); c.moveTo(padL, y1); c.lineTo(w - padR, y1); c.stroke();
    c.fillText(`${max.toFixed(max < 5 ? 1 : 0)}${unit}`, 2, y0 + 10);
    c.fillText(label, 2, y1 - 2);
    c.strokeStyle = color; c.lineWidth = 1.5; c.beginPath();
    let started = false;
    const step = Math.max(1, Math.floor(D.n / (w * 2)));
    for (let i = 0; i < D.n; i += step) {
      const v = arr[i];
      if (!isFinite(v)) { started = false; continue; }
      const x = padL + (D.t[i] / D.duration) * (w - padL - padR);
      const y = y1 - Math.min(1, v / max) * (y1 - y0);
      if (!started) { c.moveTo(x, y); started = true; } else c.lineTo(x, y);
    }
    c.stroke();
  };
  const sp = Float64Array.from({ length: D.n }, (_, i) => (D.fix[i] === 3 ? D.speed_kmh[i] : NaN));
  plot(sp, 4, mid - 4, Math.max(10, Math.ceil(D.vmax / 10) * 10), '#ff4646', '速度', 'km/h');
  plot(D.dynG, mid + 4, h - 4, Math.max(0.5, Math.ceil(D.gmax * 4) / 4), '#00cdff', 'G', 'G');
  chartGeom = { padL, padR, w };
}
let chartGeom = null;
function drawCursor(T) {
  const dpr = window.devicePixelRatio || 1;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, cv.width, cv.height);
  if (base) ctx.drawImage(base, 0, 0);
  if (!D || !chartGeom) return;
  const { padL, padR, w } = chartGeom;
  const x = (padL + (T / D.duration) * (w - padL - padR)) * dpr;
  ctx.fillStyle = '#fff'; ctx.fillRect(x - 1, 0, 2, cv.height);
}
function seekFromEvent(e) {
  if (!D || !chartGeom) return;
  const r = cv.getBoundingClientRect();
  const { padL, padR, w } = chartGeom;
  const f = (e.clientX - r.left - padL) / (w - padL - padR);
  setTime(Math.min(1, Math.max(0, f)) * D.duration);
}
let dragging = false;
cv.addEventListener('pointerdown', (e) => { dragging = true; cv.setPointerCapture(e.pointerId); seekFromEvent(e); });
cv.addEventListener('pointermove', (e) => { if (dragging) seekFromEvent(e); });
cv.addEventListener('pointerup', () => { dragging = false; });
window.addEventListener('resize', () => { layoutChart(); show(T); map.invalidateSize(); });

// ---- 再生 ----
let T = 0, playing = false, lastFrame = 0;
function setTime(t) { T = Math.min(D ? D.duration : 0, Math.max(0, t)); show(T); }
function frame(now) {
  if (!playing) return;
  T += ((now - lastFrame) / 1000) * +$('rate').value;
  lastFrame = now;
  if (T >= D.duration) { T = D.duration; setPlaying(false); }
  show(T);
  if (playing) requestAnimationFrame(frame);
}
function setPlaying(p) {
  if (p && D && T >= D.duration) T = 0;
  playing = p && !!D;
  $('play').textContent = playing ? '❚❚' : '▶';
  if (playing) { lastFrame = performance.now(); requestAnimationFrame(frame); }
}
$('play').onclick = () => setPlaying(!playing);
$('seek').oninput = (e) => setTime(+e.target.value);
$('fit').onclick = () => D && fitTrack();
document.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT' && e.target.type !== 'range' && e.target.type !== 'checkbox') return;
  if (e.code === 'Space') { e.preventDefault(); setPlaying(!playing); }
  else if (e.code === 'ArrowLeft') setTime(T - (e.shiftKey ? 30 : 5));
  else if (e.code === 'ArrowRight') setTime(T + (e.shiftKey ? 30 : 5));
});
map.on('dragstart', () => { $('follow').checked = false; });

// ---- 読み込み ----
function load(text, name) {
  try {
    setPlaying(false);
    $('notice').textContent = '';
    D = build(parseCsv(text), name);
  } catch (e) {
    D = null;
    $('notice').textContent = e.message;
    return;
  }
  const skipped = D.n - D.nvalid;
  $('meta').textContent = `${name}  ${D.n} 行 / ${fmtT(D.duration)} / 最高 ${Math.round(D.vmax)} km/h` + (skipped ? ` / 位置なし ${skipped} 行` : '');
  $('vmax').textContent = `${Math.round(D.vmax)} km/h`;
  $('seek').max = D.duration;
  heading = 0;
  T = 0;
  drawTrack();
  layoutChart();
  show(0);
}

async function loadServer(name) {
  const r = await fetch('/api/file?name=' + encodeURIComponent(name));
  if (!r.ok) { $('notice').textContent = '読み込めませんでした'; return; }
  load(await r.text(), name);
}
$('files').onchange = (e) => e.target.value && loadServer(e.target.value);
$('pick').onchange = async (e) => { const f = e.target.files[0]; if (f) load(await f.text(), f.name); };

let dragDepth = 0;
addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('drop').style.display = 'flex'; });
addEventListener('dragleave', () => { if (--dragDepth <= 0) $('drop').style.display = 'none'; });
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('drop', async (e) => {
  e.preventDefault(); dragDepth = 0; $('drop').style.display = 'none';
  const f = e.dataTransfer.files[0];
  if (f) load(await f.text(), f.name);
});

(async function init() {
  const files = await (await fetch('/api/files')).json();
  const sel = $('files');
  sel.innerHTML = '<option value="">logs/ から選ぶ…</option>' + files.map((f) => `<option value="${f.name}">${f.name} (${(f.size / 1024).toFixed(0)} KB)</option>`).join('');
  layoutChart();
  // 実走ログ(最も大きい CSV)があれば最初に開く。無ければ先頭
  const pick = files.slice().sort((a, b) => b.size - a.size)[0];
  if (pick) { sel.value = pick.name; loadServer(pick.name); }
})();
