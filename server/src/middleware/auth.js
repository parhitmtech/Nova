'use strict';
// Bearer-token (JWT) authentication for HTTP routes.
const jwt = require('jsonwebtoken');
const security = require('../services/security');
const { JWT_SECRET } = require('../config');

// ── JWT Auth Middleware ───────────────────────────────────────────────────────

function requireAuth(req, res, next) {
  const token = /^Bearer ([^\s]+)$/.exec(req.headers.authorization || "")?.[1];
  if (!token) return res.status(401).json({ error: "Unauthorized" });
  try {
    req.user = security.verifyToken(jwt, token, JWT_SECRET);
    next();
  } catch {
    res.status(401).json({ error: "Invalid token" });
  }
}

module.exports = { requireAuth };
