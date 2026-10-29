#!/usr/bin/env node
// ESP32 ログビューワー。依存なし(Leaflet は vendor/ に同梱)。
// 使い方: node server.js [ログのディレクトリ(既定: logs)] [PORT(既定: 3000)]
const http = require('http');
const fs = require('fs');
const path = require('path');

const logDir = path.resolve(process.argv[2] || path.join(__dirname, 'logs'));
const port = Number(process.argv[3] || process.env.PORT || 3000);
const roots = { public: path.join(__dirname, 'public'), vendor: path.join(__dirname, 'vendor') };

const types = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
};

function send(res, code, body, type = 'text/plain; charset=utf-8') {
  res.writeHead(code, { 'Content-Type': type });
  res.end(body);
}

function serveStatic(res, root, rel) {
  const p = path.join(root, path.normalize(rel));
  if (!p.startsWith(root + path.sep)) return send(res, 404, 'not found');
  let body;
  try {
    if (!fs.statSync(p).isFile()) return send(res, 404, 'not found');
    body = fs.readFileSync(p);
  } catch (e) {
    return send(res, e.code === 'ENOENT' ? 404 : 500, e.code === 'ENOENT' ? 'not found' : 'read error');
  }
  send(res, 200, body, types[path.extname(p)] || 'application/octet-stream');
}

const server = http.createServer((req, res) => {
  let url;
  try { url = new URL(req.url, 'http://localhost'); } catch (e) { return send(res, 400, 'bad request'); } // 例: GET //

  if (url.pathname === '/api/files') {
    const files = [];
    let names = [];
    try { names = fs.readdirSync(logDir); } catch (e) { /* ディレクトリがなければ空 */ }
    for (const f of names.filter((n) => n.toLowerCase().endsWith('.csv')).sort()) {
      try {
        const st = fs.statSync(path.join(logDir, f));
        if (st.isFile()) files.push({ name: f, size: st.size });
      } catch (e) { /* 読めない 1 件だけ除く */ }
    }
    return send(res, 200, JSON.stringify(files), 'application/json');
  }

  if (url.pathname === '/api/file') {
    const name = path.basename(url.searchParams.get('name') || '');
    const p = path.join(logDir, name);
    if (!name.toLowerCase().endsWith('.csv')) return send(res, 404, 'not found');
    try {
      if (!fs.statSync(p).isFile()) return send(res, 404, 'not found');
      return send(res, 200, fs.readFileSync(p), 'text/csv; charset=utf-8');
    } catch (e) {
      return send(res, 404, 'not found');
    }
  }

  if (url.pathname.startsWith('/vendor/')) return serveStatic(res, roots.vendor, url.pathname.slice('/vendor/'.length));
  serveStatic(res, roots.public, url.pathname === '/' ? 'index.html' : url.pathname.slice(1));
});

server.listen(port, '127.0.0.1', () => {
  console.log(`Logger viewer: http://localhost:${port}/  (logs: ${logDir})`);
});
