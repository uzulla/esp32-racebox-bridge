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

// ---- 走行距離 ----
function haversine(lat1, lon1, lat2, lon2) {
  const R = 6371008.8, r = Math.PI / 180;
  const a = Math.sin((lat2 - lat1) * r / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin((lon2 - lon1) * r / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(a));
}

// 速度(ドップラー)を時間で積分する。位置の差を足すと、静止中の GPS のゆらぎが距離として積もるため。
// FIX が 1 秒より長く途切れた区間は、前後の有効な位置の直線距離で埋める(異常に速くなるときは足さない)。
function computeDistance(d) {
  d.cum = new Float64Array(d.n);
  let total = 0, last = -1;
  for (let i = 0; i < d.n; i++) {
    if (d.valid[i]) {
      if (last >= 0) {
        const dt = d.t[i] - d.t[last];
        if (dt <= 1) {
          const v = (d.speed_kmh[i] + d.speed_kmh[last]) / 2;
          if (v >= 1) total += v / 3.6 * dt; // 1 km/h 未満は止まっているとみなす
        } else {
          const m = haversine(d.lat[last], d.lon[last], d.lat[i], d.lon[i]);
          if (m / dt < 100) total += m; // 360 km/h 超は FIX の飛びとみなす
        }
      }
      last = i;
    }
    d.cum[i] = total;
  }
  d.distance = total;
}

// ---- G ----
// 1 段階目: 静止区間ごとに「静止時の加速度 = 重力(上向き)」を取り、各行は時間的に最も近い静止区間を基準にして
//   重力を引く。取り付けや路面の傾きが途中で変わっても、次に止まった時点から合う。
// 2 段階目: GPS の速度変化(前後の加速度)と加速度センサーの向きを回帰して、車の前方向を推定し、前後 / 左右に分ける。
const vsub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const vdot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const vlen = (a) => Math.hypot(a[0], a[1], a[2]);
const vadd = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const vscale = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
const vcross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const vunit = (a) => vscale(a, 1 / (vlen(a) || 1));
const accAt = (d, i) => [d.acc_x[i], d.acc_y[i], d.acc_z[i]];

function findStationary(d) {
  const segs = [];
  const still = (i) => d.valid[i] && d.speed_kmh[i] < 1 && isFinite(d.acc_x[i] + d.acc_y[i] + d.acc_z[i]);
  for (let i = 0; i < d.n;) {
    if (!still(i)) { i++; continue; }
    let j = i;
    while (j + 1 < d.n && still(j + 1) && d.t[j + 1] - d.t[j] < 0.5) j++;
    // 前後 0.3 秒は、止まる / 動き出す途中の揺れを含むので除く
    let a = i, b = j;
    while (a < b && d.t[a] - d.t[i] < 0.3) a++;
    while (b > a && d.t[j] - d.t[b] < 0.3) b--;
    if (d.t[b] - d.t[a] >= 1.5) {
      const m = [0, 0, 0];
      for (let k = a; k <= b; k++) { m[0] += d.acc_x[k]; m[1] += d.acc_y[k]; m[2] += d.acc_z[k]; }
      const cnt = b - a + 1;
      const ref = vscale(m, 1 / cnt);
      let dev = 0; // 手で持って動かしているなど、揺れている区間は基準にしない
      for (let k = a; k <= b; k++) dev = Math.max(dev, vlen(vsub(accAt(d, k), ref)));
      if (dev < 0.06 && Math.abs(vlen(ref) - 1) < 0.1) segs.push({ a, b, t0: d.t[a], t1: d.t[b], ref });
    }
    i = j + 1;
  }
  return segs;
}

function computeG(d) {
  const n = d.n;
  d.dynG = new Float64Array(n).fill(NaN);
  d.fb = new Float64Array(n).fill(NaN);
  d.lr = new Float64Array(n).fill(NaN);
  d.hasG = false;
  d.hasAxes = false;
  d.vmax = 0;
  d.gmax = 0.5;
  for (let i = 0; i < n; i++) if (d.valid[i]) d.vmax = Math.max(d.vmax, d.speed_kmh[i]);

  const segs = findStationary(d);
  d.segCount = segs.length;
  if (!segs.length) return; // 基準が取れないと G は出せない(走行中の加速度を基準にすると狂う)
  d.hasG = true;

  // 各行 → 時間的に最も近い静止区間
  const segOf = new Int32Array(n);
  let p = 0;
  for (let i = 0; i < n; i++) {
    while (p + 1 < segs.length && segs[p + 1].t0 <= d.t[i]) p++;
    const dist = (s) => Math.max(0, s.t0 - d.t[i], d.t[i] - s.t1);
    segOf[i] = p + 1 < segs.length && dist(segs[p + 1]) < dist(segs[p]) ? p + 1 : p;
  }
  // 取り付けが同じとみなせる区間をまとめる(基準ベクトルが近い)。傾斜での停止程度の差(約 8°)は同じ取り付けとみなす
  const groups = [];
  const groupOf = segs.map((s) => {
    let g = groups.findIndex((q) => vlen(vsub(q.mean, s.ref)) < 0.15);
    if (g < 0) { groups.push({ mean: s.ref, n: 0 }); g = groups.length - 1; }
    const q = groups[g];
    q.mean = vscale(vadd(vscale(q.mean, q.n), s.ref), 1 / (q.n + 1));
    q.n++;
    return g;
  });
  d.groupCount = groups.length;

  // G(合成)
  for (let i = 0; i < n; i++) {
    const a = accAt(d, i);
    if (!isFinite(a[0] + a[1] + a[2])) continue;
    const g = vlen(vsub(a, segs[segOf[i]].ref));
    d.dynG[i] = g;
    if (g > d.gmax) d.gmax = g;
  }

  // 前後の加速度(GPS の速度変化)
  const dts = [];
  for (let i = 1; i < n; i += Math.max(1, Math.floor(n / 200))) dts.push(d.t[i] - d.t[i - 1]);
  dts.sort((x, y) => x - y);
  const k = Math.max(1, Math.round(0.4 / Math.max(0.005, dts[dts.length >> 1] || 0.04)));
  const v = Float64Array.from({ length: n }, (_, i) => (d.fix[i] === 3 ? d.speed_kmh[i] / 3.6 : NaN));
  const al = new Float64Array(n).fill(NaN);
  for (let i = k; i < n - k; i++) {
    const span = d.t[i + k] - d.t[i - k];
    if (isFinite(v[i + k] + v[i - k]) && span > 0 && span < 1.5) al[i] = (v[i + k] - v[i - k]) / span / 9.80665;
  }

  // グループごとに前方向を推定する: ah(水平の動的加速度) ≈ al × 前方向
  const acc = groups.map(() => ({ s1: [0, 0, 0], s2: 0, cnt: 0 }));
  const horiz = (i) => {
    const sg = segOf[i], u = vunit(segs[sg].ref);
    const dyn = vsub(accAt(d, i), segs[sg].ref);
    return { u, ah: vsub(dyn, vscale(u, vdot(dyn, u))), sg };
  };
  for (let i = 0; i < n; i++) {
    if (!isFinite(al[i]) || !isFinite(d.dynG[i]) || v[i] < 8 / 3.6 || Math.abs(al[i]) < 0.05) continue;
    const { u, ah, sg } = horiz(i);
    const yaw = (d.gyro_x[i] * u[0] + d.gyro_y[i] * u[1] + d.gyro_z[i] * u[2]); // 曲がっている最中は左右の加速度が混ざる
    if (!isFinite(yaw) || Math.abs(yaw) > 6) continue;
    const q = acc[groupOf[sg]];
    q.s1 = [q.s1[0] + al[i] * ah[0], q.s1[1] + al[i] * ah[1], q.s1[2] + al[i] * ah[2]];
    q.s2 += al[i] * al[i];
    q.cnt++;
  }
  const fwd = acc.map((q) => {
    if (q.cnt < 40 || q.s2 <= 0) return null;
    const f = vscale(q.s1, 1 / q.s2);
    const m = vlen(f);
    return m > 0.5 && m < 1.5 ? vunit(f) : null; // 大きさが 1 から外れる = 加速度と GPS の向きが合っていない
  });
  d.axesGroups = fwd.filter(Boolean).length;
  if (!d.axesGroups) return;
  d.hasAxes = true;
  for (let i = 0; i < n; i++) {
    if (!isFinite(d.dynG[i])) continue;
    const f0 = fwd[groupOf[segOf[i]]];
    if (!f0) continue;
    const sg = segOf[i], u = vunit(segs[sg].ref);
    const f = vunit(vsub(f0, vscale(u, vdot(f0, u)))); // 路面の傾きが違っても水平に直す
    const r = vcross(f, u); // 右
    const dyn = vsub(accAt(d, i), segs[sg].ref);
    d.fb[i] = vdot(dyn, f);
    d.lr[i] = vdot(dyn, r);
  }
}

function build(rows, name) {
  // fix は 0 / 2 / 3 だけが本物。起動直後のダミー行(fix=87 など)は捨てる
  const ok = rows.filter((r) => ['0', '2', '3'].includes(r.fix) && r.t_ms !== '' && isFinite(+r.t_ms));
  const n = ok.length;
  if (!n) throw new Error('有効な行がありません');
  // 空欄は 0 ではなく欠測(NaN)として扱う
  const num = (k) => Float64Array.from(ok, (r) => (r[k] === '' || r[k] === undefined ? NaN : +r[k]));
  // batt は Mini が整数(%)、Micro が小数 1 桁(V)。区別のため元の文字列を残す
  const d = { name, n, utc: ok.map((r) => r.utc), battRaw: ok.map((r) => r.batt) };
  for (const k of COLS.filter((c) => c !== 'utc')) d[k] = num(k);
  const t0 = d.t_ms[0];
  d.t = Float64Array.from(d.t_ms, (v) => (v - t0) / 1000);
  d.duration = d.t[n - 1];
  d.valid = new Uint8Array(n); // 位置が使える行
  d.posIdx = new Int32Array(n); // その行以前で最後に位置が使えた行
  let last = -1, nvalid = 0;
  for (let i = 0; i < n; i++) {
    if (d.fix[i] === 3 && d.hacc_m[i] < 100 && isFinite(d.speed_kmh[i]) && (d.lat[i] !== 0 || d.lon[i] !== 0) &&
        Math.abs(d.lat[i]) <= 90 && Math.abs(d.lon[i]) <= 180) { d.valid[i] = 1; last = i; nvalid++; }
    d.posIdx[i] = last;
  }
  d.nvalid = nvalid;
  d.firstValid = d.valid.indexOf(1);
  // 方位: その行以前で最後に走っていた(2 km/h 超)ときの値。シークの履歴に依らない。走り出す前は最初の値
  d.hd = new Float64Array(n).fill(NaN);
  let cur = NaN;
  for (let i = 0; i < n; i++) {
    if (d.fix[i] === 3 && d.speed_kmh[i] > 2 && isFinite(d.heading_deg[i])) cur = d.heading_deg[i];
    d.hd[i] = cur;
  }
  const firstHd = d.hd.find((v) => isFinite(v)) ?? 0;
  for (let i = 0; i < n && !isFinite(d.hd[i]); i++) d.hd[i] = firstHd;
  computeDistance(d);
  computeG(d);
  return d;
}

// ---- 表示 ----
const RAMP = [[59, 107, 255], [50, 230, 120], [255, 208, 0], [255, 70, 70]];
function speedColor(v, vmax) {
  if (!isFinite(v)) v = 0;
  const f = Math.min(1, v / Math.max(vmax, 1)) * (RAMP.length - 1);
  const i = Math.min(RAMP.length - 2, Math.floor(f)), u = f - i;
  const c = RAMP[i].map((a, k) => Math.round(a + (RAMP[i + 1][k] - a) * u));
  return `rgb(${c[0]},${c[1]},${c[2]})`;
}

// 軌跡の細かさ(Hz)。25Hz のまま全点を描くと長いログで重いので、既定は 2Hz
let trackHz = 2;
const BINS = 24; // 速度の色段階。同じ段階が続く区間は 1 本の線にまとめる

function drawPath() {
  trackLayer.clearLayers();
  if (!D.nvalid) return;
  const renderer = L.canvas({ padding: 0.5 });
  const minDt = 1 / trackHz - 1e-6;
  const kept = [];
  const breaks = new Set(); // FIX が 3 秒より長く途切れた直後の点(間引き後の間隔ではなく、元のデータの欠測で判定する)
  let lastValid = -1;
  for (let i = 0; i < D.n; i++) {
    if (!D.valid[i]) continue;
    const gap = lastValid < 0 || D.t[i] - D.t[lastValid] > 3;
    if (gap) breaks.add(i);
    if (gap || D.t[i] - D.t[kept[kept.length - 1]] >= minDt) kept.push(i);
    lastValid = i;
  }
  if (kept[kept.length - 1] !== lastValid) kept.push(lastValid);
  const binOf = (v) => Math.min(BINS - 1, Math.floor(Math.max(0, v) / Math.max(D.vmax, 1) * BINS));
  let run = null;
  const flush = () => {
    if (run && run.pts.length > 1) {
      L.polyline(run.pts, { color: speedColor((run.bin + 0.5) / BINS * D.vmax, D.vmax), weight: 5, opacity: 0.9, renderer }).addTo(trackLayer);
    }
    run = null;
  };
  for (let k = 1; k < kept.length; k++) {
    const p = kept[k - 1], i = kept[k];
    if (breaks.has(i)) { flush(); continue; } // FIX が途切れていた区間は線で結ばない
    const b = binOf(D.speed_kmh[i]);
    if (run && run.bin === b) {
      run.pts.push([D.lat[i], D.lon[i]]);
    } else {
      flush();
      run = { bin: b, pts: [[D.lat[p], D.lon[p]], [D.lat[i], D.lon[i]]] };
    }
  }
  flush();
  const s0 = kept[0], e = kept[kept.length - 1];
  L.circleMarker([D.lat[s0], D.lon[s0]], { radius: 6, color: '#fff', fillColor: '#32e678', fillOpacity: 1, weight: 2 }).bindTooltip('START').addTo(trackLayer);
  L.circleMarker([D.lat[e], D.lon[e]], { radius: 6, color: '#fff', fillColor: '#ff4646', fillOpacity: 1, weight: 2 }).bindTooltip('END').addTo(trackLayer);
}

function drawTrack() {
  trackLayer.clearLayers();
  if (marker) { map.removeLayer(marker); marker = null; }
  if (!D.nvalid) { $('notice').textContent = '位置(3D FIX)が取れていないログです。地図は表示できません'; return; }
  drawPath();
  const icon = L.divIcon({
    className: '', iconSize: [28, 28], iconAnchor: [14, 14],
    html: '<svg class="arrow" viewBox="-14 -14 28 28"><path d="M0-11 L8 9 L0 5 L-8 9 Z" fill="#00cdff" stroke="#fff" stroke-width="2" stroke-linejoin="round"/></svg>',
  });
  marker = L.marker([D.lat[D.firstValid], D.lon[D.firstValid]], { icon, interactive: false, keyboard: false, zIndexOffset: 1000 }).addTo(map);
  fitTrack();
}

function clearView() {
  trackLayer.clearLayers();
  if (marker) { map.removeLayer(marker); marker = null; }
  for (const id of ['g', 'acc', 'hdg', 'alt', 'gps', 'hacc', 'yaw', 'batt', 'clock']) $(id).textContent = '--';
  $('speed').firstChild.nodeValue = '--';
  $('gbar').style.width = '0';
  $('dist').textContent = '--'; $('fblr').textContent = '--';
  $('pos').textContent = '0:00 / 0:00';
  $('seek').max = 0; $('seek').value = 0;
  $('vmax').textContent = '-- km/h';
  $('meta').textContent = 'CSV を選ぶか、ここにドロップしてください';
  layoutChart();
  drawCursor(0);
  drawBall(0);
}

function fitTrack() {
  const ll = [];
  for (let i = 0; i < D.n; i++) if (D.valid[i]) ll.push([D.lat[i], D.lon[i]]);
  if (ll.length) map.fitBounds(L.latLngBounds(ll), { padding: [40, 40], maxZoom: 18 });
}

const fmtKm = (m) => (m < 1000 ? `${Math.round(m)} m` : `${(m / 1000).toFixed(2)} km`);
const signed = (v) => (v >= 0 ? '+' : '−') + Math.abs(v).toFixed(2);
const fmtT = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
function jst(utc) {
  const m = /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)/.exec(utc);
  if (!m || +m[1] < 2020) return null;
  const t = new Date(Date.UTC(+m[1], m[2] - 1, +m[3], +m[4], +m[5], +m[6]) + 9 * 3600e3);
  return t.toISOString().slice(0, 19).replace('T', ' ') + ' JST';
}

function upperBound(arr, x) { let lo = 0, hi = arr.length; while (lo < hi) { const m = (lo + hi) >> 1; if (arr[m] <= x) lo = m + 1; else hi = m; } return lo; }

let arrowRot = 0; // 矢印の累積角度(北をまたいでも逆回りに一周しないよう、差を ±180° に畳んで足す)
function show(T) {
  if (!D) return;
  const i = Math.max(0, upperBound(D.t, T) - 1);
  const pi = D.posIdx[i] >= 0 ? D.posIdx[i] : D.firstValid; // FIX 前は最初の有効位置に置く
  const fix3 = D.fix[i] === 3;
  $('speed').firstChild.nodeValue = fix3 ? Math.round(D.speed_kmh[i]) : '--';
  $('clock').textContent = jst(D.utc[i]) || '--';
  $('g').textContent = D.hasG ? D.dynG[i].toFixed(2) + ' G' : '--';
  $('dist').textContent = `${fmtKm(D.cum[i])} / ${fmtKm(D.distance)}`;
  $('fblr').textContent = isFinite(D.fb[i]) ? `${signed(D.fb[i])} / ${signed(D.lr[i])} G` : '--';
  drawBall(i);
  $('gbar').style.width = D.hasG ? Math.min(100, D.dynG[i] / Math.max(1.5, D.gmax) * 100) + '%' : '0';
  $('acc').textContent = `${D.acc_x[i].toFixed(2)} / ${D.acc_y[i].toFixed(2)} / ${D.acc_z[i].toFixed(2)}`;
  $('alt').textContent = fix3 ? D.alt_m[i].toFixed(0) + ' m' : '--';
  $('gps').textContent = fix3 ? `3D FIX ${D.sats[i]} 衛星` : D.fix[i] === 2 ? `2D FIX ${D.sats[i]} 衛星` : 'FIX なし';
  $('hacc').textContent = fix3 ? D.hacc_m[i].toFixed(1) + ' m' : '--';
  $('yaw').textContent = D.gyro_z[i].toFixed(1) + ' °/s';
  const braw = D.battRaw[i];
  $('batt').textContent = braw.includes('.') ? `${(+braw).toFixed(1)} V` : `${braw} %` + (D.charging[i] ? ' 充電中' : '');
  const heading = D.hd[i];
  $('hdg').textContent = fix3 ? `${Math.round(heading)}°` : '--';
  $('pos').textContent = `${fmtT(T)} / ${fmtT(D.duration)}`;
  $('seek').value = T;
  if (marker && pi >= 0) {
    const ll = [D.lat[pi], D.lon[pi]];
    marker.setLatLng(ll);
    const el = marker.getElement()?.firstChild;
    arrowRot += ((heading - arrowRot) % 360 + 540) % 360 - 180;
    if (el) el.style.transform = `rotate(${arrowRot}deg)`;
    if ($('follow').checked && !map.getBounds().pad(-0.3).contains(ll)) map.panTo(ll, { animate: false });
  }
  drawCursor(T);
}

// ---- G ボール(上 = 加速、下 = 減速、右 = 右向きの加速度 = 右コーナー。直近 1.5 秒の軌跡つき) ----
const ballCv = $('ball'), bctx = ballCv.getContext('2d');
function drawBall(i) {
  const dpr = window.devicePixelRatio || 1;
  const size = Math.round(ballCv.clientWidth * dpr);
  if (size && ballCv.width !== size) { ballCv.width = size; ballCv.height = size; }
  const W = ballCv.width, c = W / 2, R = W / 2 - 4 * dpr, scale = R / 1.5;
  bctx.clearRect(0, 0, W, W);
  bctx.lineWidth = dpr;
  for (const g of [0.5, 1, 1.5]) {
    bctx.strokeStyle = g === 1 ? '#ff4646' : '#26304d';
    bctx.beginPath(); bctx.arc(c, c, g * scale, 0, 2 * Math.PI); bctx.stroke();
  }
  bctx.strokeStyle = '#26304d';
  bctx.beginPath(); bctx.moveTo(c, c - R); bctx.lineTo(c, c + R); bctx.moveTo(c - R, c); bctx.lineTo(c + R, c); bctx.stroke();
  if (!D || !isFinite(D.fb[i])) return;
  for (let j = i; j >= 0 && D.t[i] - D.t[j] < 1.5; j -= 2) {
    if (!isFinite(D.fb[j])) continue;
    bctx.fillStyle = `rgba(0,205,255,${0.6 * (1 - (D.t[i] - D.t[j]) / 1.5)})`;
    bctx.beginPath(); bctx.arc(c + D.lr[j] * scale, c - D.fb[j] * scale, 2.5 * dpr, 0, 2 * Math.PI); bctx.fill();
  }
  bctx.fillStyle = D.dynG[i] > 1 ? '#ff4646' : '#00cdff';
  bctx.strokeStyle = '#fff'; bctx.lineWidth = 2 * dpr;
  bctx.beginPath(); bctx.arc(c + D.lr[i] * scale, c - D.fb[i] * scale, 6 * dpr, 0, 2 * Math.PI); bctx.fill(); bctx.stroke();
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
    const yOf = (v) => y1 - Math.min(1, v / max) * (y1 - y0);
    for (let i = 0; i < D.n; i += step) {
      // 間引くときも区間の最小・最大を残す(短いピークが消えないように)
      let lo = Infinity, hi = -Infinity;
      for (let j = i; j < Math.min(D.n, i + step); j++) {
        const v = arr[j];
        if (isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; }
      }
      if (lo === Infinity) { started = false; continue; }
      const x = padL + (D.t[i] / D.duration) * (w - padL - padR);
      if (!started) { c.moveTo(x, yOf(lo)); started = true; } else c.lineTo(x, yOf(lo));
      if (hi !== lo) c.lineTo(x, yOf(hi));
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
  loadSeq++; // 保留中のサーバー読み込みを無効にする
  try {
    setPlaying(false);
    $('notice').textContent = '';
    D = build(parseCsv(text), name);
  } catch (e) {
    D = null;
    clearView();
    $('notice').textContent = e.message;
    return;
  }
  const skipped = D.n - D.nvalid;
  $('meta').textContent = `${name}  ${D.n} 行 / ${fmtT(D.duration)} / 最高 ${Math.round(D.vmax)} km/h / ${fmtKm(D.distance)}` + (skipped ? ` / 位置なし ${skipped} 行` : '');
  if (!D.hasG) $('notice').textContent = '静止区間が無いため、G は表示しません';
  else if (!D.hasAxes) $('notice').textContent = '直進での加減速が足りないため、前後 / 左右には分けません(合成 G のみ)';
  $('vmax').textContent = `${Math.round(D.vmax)} km/h`;
  $('seek').max = D.duration;
  arrowRot = D.hd[0];
  T = 0;
  drawTrack();
  layoutChart();
  show(0);
}

// 選び直したとき、遅れて届いた古い応答で上書きしないよう連番で最新だけ反映する
let loadSeq = 0;
async function loadServer(name) {
  const seq = ++loadSeq;
  try {
    const r = await fetch('/api/file?name=' + encodeURIComponent(name));
    if (seq !== loadSeq) return;
    if (!r.ok) { $('notice').textContent = '読み込めませんでした'; return; }
    const text = await r.text();
    if (seq !== loadSeq) return;
    load(text, name);
  } catch (e) {
    if (seq === loadSeq) $('notice').textContent = '読み込めませんでした';
  }
}
$('files').onchange = (e) => e.target.value && loadServer(e.target.value);
// await 中に別のファイルが選ばれたら、遅れて解決した古い方は捨てる
async function loadFile(f) {
  const seq = ++loadSeq;
  const text = await f.text();
  if (seq === loadSeq) load(text, f.name);
}
$('pick').onchange = (e) => { const f = e.target.files[0]; e.target.value = ''; if (f) loadFile(f); };
$('density').onchange = (e) => { trackHz = +e.target.value; if (D && D.nvalid) drawPath(); };

let dragDepth = 0;
addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('drop').style.display = 'flex'; });
addEventListener('dragleave', () => { if (--dragDepth <= 0) $('drop').style.display = 'none'; });
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('drop', async (e) => {
  e.preventDefault(); dragDepth = 0; $('drop').style.display = 'none';
  const f = e.dataTransfer.files[0];
  if (f) loadFile(f);
});

(async function init() {
  const files = await (await fetch('/api/files')).json();
  const sel = $('files');
  sel.replaceChildren(new Option('logs/ から選ぶ…', ''), ...files.map((f) => new Option(`${f.name} (${(f.size / 1024).toFixed(0)} KB)`, f.name)));
  layoutChart();
  // 実走ログ(最も大きい CSV)があれば最初に開く。無ければ先頭
  const pick = files.slice().sort((a, b) => b.size - a.size)[0];
  if (pick) { sel.value = pick.name; loadServer(pick.name); }
})();
