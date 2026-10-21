'use strict';
// WebSocket upgrade routing for /repl and /terminal: origin checks, first-frame token authentication and socket limits.
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
  let pendingSockets = 0;
  server.on('upgrade', (req, socket, head) => {
    const reject = status => { socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\n\r\n`); };
    let url;
    try { url = new URL(req.url, 'http://localhost'); } catch { return reject('400 Bad Request'); }
    if (!['/repl', '/terminal'].includes(url.pathname)) return reject('404 Not Found');
    if (!req.headers.origin || !security.originAllowed(req.headers.origin, req)) return reject('403 Forbidden');
    // Credentials in URLs can be recorded by HTTP proxies. Require an auth frame.
    if (url.searchParams.has('token')) return reject('401 Unauthorized');
    if (pendingSockets >= 20 || wss.clients.size + termWss.clients.size >= 40) return reject('503 Service Unavailable');
    const target = url.pathname === '/repl' ? wss : termWss;
    target.handleUpgrade(req, socket, head, ws => {
      pendingSockets++;
      let pending = true;
      const release = () => { if (pending) { pending = false; pendingSockets--; } };
      const timer = setTimeout(() => ws.terminate(), 5000);
      timer.unref();
      ws.once('close', () => { clearTimeout(timer); release(); });
      ws.on('error', () => {});
      // The first frame must be { type: 'auth', token }; nothing starts before it validates.
      ws.once('message', raw => {
        clearTimeout(timer); release();
        let claims;
        try {
          const msg = JSON.parse(raw.toString());
          if (msg.type !== 'auth' || typeof msg.token !== 'string') throw new Error('Authentication required');
          req.user = security.verifyToken(jwt, msg.token, JWT_SECRET);
          claims = jwt.decode(msg.token);
        } catch { ws.close(4001, 'Invalid authentication'); return; }
        const id = req.user.userId;
        req._userId = id;
        const now = Date.now();
        for (const [key, value] of upgradeRates) if (now - value.start > 60000) upgradeRates.delete(key);
        const rate = upgradeRates.get(id) || { start: now, count: 0 };
        rate.count++; upgradeRates.set(id, rate);
        if (rate.count > 10 || (socketCounts.get(id) || 0) >= 3 || [...socketCounts.values()].reduce((a, b) => a + b, 0) >= 20) {
          ws.close(1013, 'Session limit exceeded'); return;
        }
        socketCounts.set(id, (socketCounts.get(id) || 0) + 1);
        ws.once('close', () => { const n = (socketCounts.get(id) || 1) - 1; n ? socketCounts.set(id, n) : socketCounts.delete(id); });
        // Expiration applies to an established connection as well as its handshake.
        const expiry = setTimeout(() => ws.close(4001, 'Session expired'), Math.max(1, claims.exp * 1000 - Date.now()));
        expiry.unref(); ws.once('close', () => clearTimeout(expiry));
        target.emit('connection', ws, req);
      });
    });
  });
}

module.exports = { attachWebSockets };
