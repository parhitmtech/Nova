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

// Unauthenticated sockets are bounded globally and per client IP, and are
// dropped after AUTH_TIMEOUT_MS; only authenticated sockets count toward the
// session limits, so anonymous clients cannot lock signed-in users out.
const MAX_PENDING = 64;
const MAX_PENDING_PER_IP = 4;
const AUTH_TIMEOUT_MS = 5000;
const MAX_SESSIONS = 20;
const MAX_SESSIONS_PER_USER = 3;
const MAX_UPGRADES_PER_USER_PER_MINUTE = 10;

// The client address as Express computes req.ip: walk X-Forwarded-For from
// the nearest hop only while each hop is a trusted proxy.
function clientIp(req, trust) {
  const forwarded = String(req.headers['x-forwarded-for'] || '').split(',').map(s => s.trim()).filter(Boolean).reverse();
  const addrs = [req.socket.remoteAddress, ...forwarded];
  let i = 0;
  while (trust && i < addrs.length - 1 && trust(addrs[i], i)) i++;
  return addrs[i] || 'unknown';
}

// trustProxy: Express' compiled 'trust proxy' function (app.get('trust proxy fn')).
function attachWebSockets(server, { trustProxy } = {}) {
  const wss     = new WebSocket.Server({ noServer: true, maxPayload: 16384 });  // REPL
  const termWss = new WebSocket.Server({ noServer: true, maxPayload: 16384 });  // Terminal
  wss.on('connection', handleReplConnection);
  termWss.on('connection', handleTerminalConnection);

  const socketCounts = new Map();   // userId → authenticated sockets
  const upgradeRates = new Map();   // userId → { start, count }
  const pendingByIp = new Map();    // client IP → unauthenticated sockets
  let pendingSockets = 0;
  let authenticatedSockets = 0;

  server.on('upgrade', (req, socket, head) => {
    const reject = status => { socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\n\r\n`); };
    let url;
    try { url = new URL(req.url, 'http://localhost'); } catch { return reject('400 Bad Request'); }
    if (!['/repl', '/terminal'].includes(url.pathname)) return reject('404 Not Found');
    if (!req.headers.origin || !security.originAllowed(req.headers.origin, req)) return reject('403 Forbidden');
    // Credentials in URLs can be recorded by HTTP proxies. Require an auth frame.
    if (url.searchParams.has('token')) return reject('401 Unauthorized');
    const ip = clientIp(req, trustProxy);
    if ((pendingByIp.get(ip) || 0) >= MAX_PENDING_PER_IP) return reject('429 Too Many Requests');
    if (pendingSockets >= MAX_PENDING || authenticatedSockets >= MAX_SESSIONS) return reject('503 Service Unavailable');
    const target = url.pathname === '/repl' ? wss : termWss;
    target.handleUpgrade(req, socket, head, ws => {
      // Counted only once the handshake succeeded: malformed upgrades hold no slot.
      pendingSockets++;
      pendingByIp.set(ip, (pendingByIp.get(ip) || 0) + 1);
      let pending = true;
      const release = () => {
        if (!pending) return;
        pending = false; pendingSockets--;
        const n = (pendingByIp.get(ip) || 1) - 1;
        n ? pendingByIp.set(ip, n) : pendingByIp.delete(ip);
      };
      // terminate() drops the TCP connection at once instead of waiting for a close handshake.
      const timer = setTimeout(() => ws.terminate(), AUTH_TIMEOUT_MS);
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
        } catch { ws.terminate(); return; }
        const id = req.user.userId;
        req._userId = id;
        const now = Date.now();
        for (const [key, value] of upgradeRates) if (now - value.start > 60000) upgradeRates.delete(key);
        const rate = upgradeRates.get(id) || { start: now, count: 0 };
        rate.count++; upgradeRates.set(id, rate);
        if (rate.count > MAX_UPGRADES_PER_USER_PER_MINUTE || (socketCounts.get(id) || 0) >= MAX_SESSIONS_PER_USER || authenticatedSockets >= MAX_SESSIONS) {
          ws.terminate(); return;
        }
        authenticatedSockets++;
        socketCounts.set(id, (socketCounts.get(id) || 0) + 1);
        ws.once('close', () => {
          authenticatedSockets--;
          const n = (socketCounts.get(id) || 1) - 1;
          n ? socketCounts.set(id, n) : socketCounts.delete(id);
        });
        // Expiration applies to an established connection as well as its handshake.
        const expiry = setTimeout(() => ws.close(4001, 'Session expired'), Math.max(1, claims.exp * 1000 - Date.now()));
        expiry.unref(); ws.once('close', () => clearTimeout(expiry));
        target.emit('connection', ws, req);
      });
    });
  });
}

module.exports = { attachWebSockets, clientIp };
