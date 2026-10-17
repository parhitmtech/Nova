'use strict';
// WebSocket upgrade routing for /repl and /terminal: origin + token checks and per-user socket limits.
//
// Using noServer + manual upgrade routing avoids the conflict that occurs when
// two WebSocket.Server instances both attach upgrade listeners to the same
// http.Server (they can intercept each other's connections).
const WebSocket = require('ws');
const jwt = require('jsonwebtoken');
const security = require('../services/security');
const { JWT_SECRET } = require('../config');
const { handleReplConnection } = require('./repl');
const { handleTerminalConnection } = require('./terminal');

function attachWebSockets(server) {
  const wss     = new WebSocket.Server({ noServer: true, maxPayload: 16384 });  // REPL
  const termWss = new WebSocket.Server({ noServer: true, maxPayload: 16384 });  // Terminal
  wss.on('connection', handleReplConnection);
  termWss.on('connection', handleTerminalConnection);

  const socketCounts = new Map();
  const upgradeRates = new Map();
  server.on('upgrade', (req, socket, head) => {
    const reject = status => { socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\n\r\n`); };
    const url = new URL(req.url, 'http://localhost');
    if (!['/repl', '/terminal'].includes(url.pathname)) return reject('404 Not Found');
    if (!req.headers.origin || !security.originAllowed(req.headers.origin, req)) return reject('403 Forbidden');
    try { req.user = security.verifyToken(jwt, url.searchParams.get('token') || '', JWT_SECRET); }
    catch { return reject('401 Unauthorized'); }
    req._userId = req.user.userId;
    const id = req._userId;
    for (const [key, value] of upgradeRates) if (Date.now() - value.start > 60000) upgradeRates.delete(key);
    const rate = upgradeRates.get(id);
    const now = Date.now();
    const window = !rate || now - rate.start > 60000 ? { start: now, count: 0 } : rate;
    window.count++; upgradeRates.set(id, window);
    if (window.count > 10 || (socketCounts.get(id) || 0) >= 3 || [...socketCounts.values()].reduce((a, b) => a + b, 0) >= 20) return reject('429 Too Many Requests');
    const target = url.pathname === '/repl' ? wss : termWss;
    target.handleUpgrade(req, socket, head, ws => {
      socketCounts.set(id, (socketCounts.get(id) || 0) + 1);
      ws.once('close', () => { const n = (socketCounts.get(id) || 1) - 1; n ? socketCounts.set(id, n) : socketCounts.delete(id); });
      target.emit('connection', ws, req);
    });
  });
}

module.exports = { attachWebSockets };
