'use strict';
// ラップ検出。コースの事前情報なしで、軌跡だけから計測ライン(ゲート)を見つけて周回時間を出す。
//   1. 走行中の点を候補にして、その点で進行方向に直交する短い線(ゲート)を置く
//   2. ゲートを同じ向きに横切った時刻を数える(2 点間を線形補間)
//   3. 周回間隔が揃っていて回数が多いゲートを選ぶ(周回コースならどの点でも同じ周期になる)
// 入力: {n, t(秒), lat, lon, speed(km/h), valid(Uint8Array, 省略可)}
(function (root) {
  const GATE_HALF = 12;      // m。ゲートの半幅(コース幅より広く、隣の区間にかからない程度)
  const MIN_SPEED = 20;      // km/h。ゲート候補に使う最低速度(ピットや低速区間を避ける)
  const CAND_STEP = 1.0;     // 秒。候補点の間隔
  const MIN_LAP = 15;        // 秒。これより短い間隔は同じ通過のダブり(ゲートのふらつき)として捨てる
  const R = 6371000;

  function project(d) {
    let lat0 = 0, lon0 = 0, c = 0;
    for (let i = 0; i < d.n; i++) if (!d.valid || d.valid[i]) { lat0 += d.lat[i]; lon0 += d.lon[i]; c++; }
    lat0 /= c; lon0 /= c;
    const kx = R * Math.cos(lat0 * Math.PI / 180) * Math.PI / 180, ky = R * Math.PI / 180;
    const x = new Float64Array(d.n), y = new Float64Array(d.n);
    for (let i = 0; i < d.n; i++) { x[i] = (d.lon[i] - lon0) * kx; y[i] = (d.lat[i] - lat0) * ky; }
    return { x, y, lat0, lon0, kx, ky };
  }

  // 点 (px,py)、進行方向 (hx,hy) のゲートを同じ向きに横切る時刻を返す。
  // ks(idx 上の位置の昇順の配列)を渡すと、その位置で終わる線分だけを調べる。省略時は全部
  function crossings(d, P, idx, px, py, hx, hy, half, ks) {
    const out = [];
    const { x, y } = P;
    const test = (k) => {
      const prevI = idx[k - 1], i = idx[k];
      if (!(d.t[i] - d.t[prevI] < 2)) return;
      const prevS = (x[prevI] - px) * hx + (y[prevI] - py) * hy; // 進行方向の距離(ゲートからの符号付き距離)
      const s = (x[i] - px) * hx + (y[i] - py) * hy;
      if (prevS < 0 && s >= 0) {
        const f = -prevS / (s - prevS);
        const cx = x[prevI] + (x[i] - x[prevI]) * f, cy = y[prevI] + (y[i] - y[prevI]) * f;
        const lat = -(cx - px) * hy + (cy - py) * hx; // ゲート上の横位置
        if (Math.abs(lat) <= half) out.push({ t: d.t[prevI] + (d.t[i] - d.t[prevI]) * f, off: lat });
      }
    };
    if (ks) for (let j = 0; j < ks.length; j++) test(ks[j]);
    else for (let k = 1; k < idx.length; k++) test(k);
    return out;
  }

  // 線分(idx 上で隣り合う 2 点)を升目に入れておき、ゲートの近くの線分だけを調べられるようにする。
  // 候補ごとに全点を見ると、長いログ(十数万点)で読み込みが数秒止まるため
  const CELL = 50; // m
  function buildGrid(d, P, idx) {
    const { x, y } = P;
    let minX = Infinity, minY = Infinity, maxY = -Infinity;
    for (const i of idx) { if (x[i] < minX) minX = x[i]; if (y[i] < minY) minY = y[i]; if (y[i] > maxY) maxY = y[i]; }
    const ny = Math.floor((maxY - minY) / CELL) + 1;
    const cx = (v) => Math.floor((v - minX) / CELL), cy = (v) => Math.floor((v - minY) / CELL);
    const cells = new Map();
    for (let k = 1; k < idx.length; k++) {
      const a = idx[k - 1], b = idx[k];
      if (!(d.t[b] - d.t[a] < 2)) continue;
      const x0 = cx(Math.min(x[a], x[b])), x1 = cx(Math.max(x[a], x[b])), y0 = cy(Math.min(y[a], y[b])), y1 = cy(Math.max(y[a], y[b]));
      for (let u = x0; u <= x1; u++) for (let v = y0; v <= y1; v++) {
        const key = u * ny + v;
        const c = cells.get(key);
        if (c) c.push(k); else cells.set(key, [k]);
      }
    }
    // (px,py) から half 以内を通りうる線分の位置を、昇順・重複なしで返す
    return (px, py, half) => {
      const x0 = cx(px - half), x1 = cx(px + half), y0 = Math.max(0, cy(py - half)), y1 = Math.min(ny - 1, cy(py + half));
      let ks = null, many = false;
      for (let u = x0; u <= x1; u++) for (let v = y0; v <= y1; v++) {
        const c = cells.get(u * ny + v);
        if (!c) continue;
        if (!ks) ks = c; else { ks = ks.concat(c); many = true; }
      }
      if (!ks) return [];
      if (!many) return ks;
      ks.sort((p, q) => p - q);
      return ks.filter((k, j) => j === 0 || k !== ks[j - 1]);
    };
  }

  function validIdx(d) {
    const idx = [];
    for (let i = 0; i < d.n; i++) if (!d.valid || d.valid[i]) idx.push(i);
    return idx;
  }

  // 向き: 前後 1 秒程度の位置差から求める(GPS の方位は低速でぶれる)
  function headingAt(d, P, idx, k) {
    let a = k, b = k;
    while (a > 0 && d.t[idx[k]] - d.t[idx[a]] < 1) a--;
    while (b < idx.length - 1 && d.t[idx[b]] - d.t[idx[k]] < 1) b++;
    const dx = P.x[idx[b]] - P.x[idx[a]], dy = P.y[idx[b]] - P.y[idx[a]];
    const l = Math.hypot(dx, dy);
    return l > 3 ? [dx / l, dy / l] : null;
  }

  function lapsFromCrossings(cs) {
    const laps = [];
    for (let i = 1; i < cs.length; i++) laps.push({ start: cs[i - 1].t, end: cs[i].t, time: cs[i].t - cs[i - 1].t });
    return laps;
  }

  // 直前の通過から MIN_LAP 秒以内の通過は、同じ通過のダブりとして捨てる
  function dropRepeats(cs) {
    const out = [];
    for (const c of cs) if (!out.length || c.t - out[out.length - 1].t >= MIN_LAP) out.push(c);
    return out;
  }

  function scoreGate(cs) {
    // 最頻の周期に近い周回の数を点数にする
    const gaps = [];
    for (let i = 1; i < cs.length; i++) gaps.push(cs[i].t - cs[i - 1].t);
    if (gaps.length < 2) return null;
    const sorted = gaps.slice().sort((a, b) => a - b);
    const med = sorted[sorted.length >> 1];
    const near = gaps.filter((g) => Math.abs(g - med) < med * 0.25);
    const dev = near.reduce((s, g) => s + Math.abs(g - med), 0) / near.length;
    // 周回数が多く、周期のばらつきが小さく、横位置のばらつきが小さいほど良い
    const offs = cs.map((c) => c.off);
    const m = offs.reduce((s, v) => s + v, 0) / offs.length;
    const sd = Math.sqrt(offs.reduce((s, v) => s + (v - m) * (v - m), 0) / offs.length);
    return { med, nNear: near.length, n: cs.length, dev, sd, score: near.length * 10 - dev - sd };
  }

  // 点 k の前後 dist m の位置から、進行方向の曲がり具合(度)を出す。直線なら 0 に近い
  function bendAt(d, P, idx, cum, k, dist) {
    let a = k, b = k;
    while (a > 0 && cum[k] - cum[a] < dist) a--;
    while (b < idx.length - 1 && cum[b] - cum[k] < dist) b++;
    if (cum[k] - cum[a] < dist * 0.8 || cum[b] - cum[k] < dist * 0.8) return null;
    if (d.t[idx[b]] - d.t[idx[a]] > 30) return null; // 途切れをまたぐ
    const i = idx[k], ia = idx[a], ib = idx[b];
    const a1 = Math.atan2(P.y[i] - P.y[ia], P.x[i] - P.x[ia]), a2 = Math.atan2(P.y[ib] - P.y[i], P.x[ib] - P.x[i]);
    let da = Math.abs(a2 - a1);
    if (da > Math.PI) da = 2 * Math.PI - da;
    return da * 180 / Math.PI;
  }

  // スタートラインは直線上にあることが多い。周回数が十分取れるゲートのうち、前後が最も直線に近い点を選ぶ。
  // 直線の途中でも向きが揺れるので、近い距離(30 m)と遠い距離(80 m)の曲がりを合わせて見る。
  function detect(d, opts) {
    opts = opts || {};
    const half = opts.half || GATE_HALF;
    const P = project(d);
    const idx = validIdx(d);
    const cum = new Float64Array(idx.length); // 走行距離(位置の差の積算)
    for (let k = 1; k < idx.length; k++) cum[k] = cum[k - 1] + Math.hypot(P.x[idx[k]] - P.x[idx[k - 1]], P.y[idx[k]] - P.y[idx[k - 1]]);
    const near = buildGrid(d, P, idx);
    const cands = [];
    let nextT = -1e9;
    for (let k = 0; k < idx.length; k++) {
      const i = idx[k];
      if (d.t[i] < nextT) continue;
      if (d.speed[i] < MIN_SPEED) continue;
      nextT = d.t[i] + CAND_STEP;
      const h = headingAt(d, P, idx, k);
      if (!h) continue;
      const cs = dropRepeats(crossings(d, P, idx, P.x[i], P.y[i], h[0], h[1], half, near(P.x[i], P.y[i], half)));
      if (cs.length < 3) continue;
      const sc = scoreGate(cs);
      if (!sc) continue;
      const b1 = bendAt(d, P, idx, cum, k, 30), b2 = bendAt(d, P, idx, cum, k, 80);
      if (b1 === null || b2 === null) continue;
      cands.push({ x: P.x[i], y: P.y[i], hx: h[0], hy: h[1], cs, sc, bend: b1 + b2, speed: d.speed[i] });
    }
    if (!cands.length) return null;
    const maxNear = Math.max(...cands.map((c) => c.sc.nNear));
    // 周回が取れている(最多の 9 割以上)ゲートだけを対象に、最も直線的な点を選ぶ。曲がりが同程度なら速い方
    const ok = cands.filter((c) => c.sc.nNear >= Math.max(3, maxNear * 0.9));
    let best = ok[0];
    for (const c of ok) if (c.bend < best.bend - 1 || (Math.abs(c.bend - best.bend) <= 1 && c.speed > best.speed)) best = c;
    const lat = best.y / P.ky + P.lat0, lon = best.x / P.kx + P.lon0;
    const nx = -best.hy, ny = best.hx;
    const end = (s) => ({ lat: (best.y + ny * half * s) / P.ky + P.lat0, lon: (best.x + nx * half * s) / P.kx + P.lon0 });
    const laps = lapsFromCrossings(best.cs);
    return { gate: { lat, lon, heading: Math.atan2(best.hx, best.hy) * 180 / Math.PI, a: end(1), b: end(-1), half, bend: best.bend }, crossings: best.cs, laps, score: best.sc };
  }

  const api = { detect, project, crossings };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.Lap = api;
})(typeof self !== 'undefined' ? self : this);
