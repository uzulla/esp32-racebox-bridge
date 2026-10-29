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
  if (!p.startsWith(root + path.sep) || !fs.existsSync(p) || !fs.statSync(p).isFile()) return send(res, 404, 'not found');
  send(res, 200, fs.readFileSync(p), types[path.extname(p)] || 'application/octet-stream');
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');

  if (url.pathname === '/api/files') {
    let files = [];
    try {
      files = fs.readdirSync(logDir)
        .filter((f) => f.toLowerCase().endsWith('.csv'))
        .sort()
        .map((f) => ({ name: f, size: fs.statSync(path.join(logDir, f)).size }));
    } catch (e) { /* ディレクトリがなければ空 */ }
    return send(res, 200, JSON.stringify(files), 'application/json');
  }

  if (url.pathname === '/api/file') {
    const name = path.basename(url.searchParams.get('name') || '');
    const p = path.join(logDir, name);
    if (!name.toLowerCase().endsWith('.csv') || !fs.existsSync(p)) return send(res, 404, 'not found');
    return send(res, 200, fs.readFileSync(p), 'text/csv; charset=utf-8');
  }

  if (url.pathname.startsWith('/vendor/')) return serveStatic(res, roots.vendor, url.pathname.slice('/vendor/'.length));
  serveStatic(res, roots.public, url.pathname === '/' ? 'index.html' : url.pathname.slice(1));
});

server.listen(port, '127.0.0.1', () => {
  console.log(`Logger viewer: http://localhost:${port}/  (logs: ${logDir})`);
});
