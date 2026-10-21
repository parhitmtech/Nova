'use strict';
const fs = require('node:fs');
const path = require('node:path');
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const JWT_OPTIONS = { algorithms: ['HS256'], issuer: 'nova', audience: 'nova-ide' };
function validateSecret(secret) {
  if (typeof secret !== 'string' || Buffer.byteLength(secret) < 32 || new Set(secret).size < 12 || /novacomp-secret|change.?me|example|placeholder/i.test(secret))
    throw new Error('JWT_SECRET must be a strong random secret of at least 32 bytes');
  return secret;
}
function verifyToken(jwt, token, secret) {
  const user = jwt.verify(token, secret, JWT_OPTIONS);
  if (!user || !UUID.test(user.userId || '') || typeof user.email !== 'string' || user.email.length > 254 || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(user.email) || !Number.isInteger(user.exp) || !Number.isInteger(user.iat))
    throw new Error('Invalid token claims');
  return { userId: user.userId, email: user.email };
}
function inside(root, target) { return target === root || target.startsWith(root + path.sep); }
// Resolve the trusted root, then reject symlinks only beneath that boundary
// (including dangling links).
function safePath(root, relative = '', { allowRoot = false, regularFile = false } = {}) {
  if (typeof relative !== 'string' || relative.includes('\0')) return null;
  try {
    const lexicalRoot = path.resolve(root);
    root = fs.realpathSync(lexicalRoot);
    if (path.isAbsolute(relative) && inside(lexicalRoot, relative)) relative = path.relative(lexicalRoot, relative);
    const target = path.resolve(root, relative);
    if (!inside(root, target) || (!allowRoot && target === root)) return null;
    let current = root;
    for (const part of path.relative(root, target).split(path.sep).filter(Boolean)) {
      current = path.join(current, part);
      try { if (fs.lstatSync(current).isSymbolicLink()) return null; }
      catch (e) { if (e.code !== 'ENOENT') return null; }
    }
    if (regularFile && !fs.lstatSync(target).isFile()) return null;
    return target;
  } catch { return null; }
}
function userWorkspace(root, userId) {
  if (!UUID.test(userId || '')) throw new Error('Invalid user identity');
  const dir = safePath(root, userId);
  if (!dir) throw new Error('Invalid workspace');
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
  return dir;
}
function originAllowed(origin, req, configured = process.env.NOVA_ALLOWED_ORIGINS || '') {
  if (!origin) return true; // API clients still require authentication.
  let parsed;
  try { parsed = new URL(origin); } catch { return false; }
  if (!['http:', 'https:'].includes(parsed.protocol) || parsed.origin !== origin) return false;
  const allowed = configured.split(',').map(s => s.trim()).filter(Boolean);
  // A configured list is exclusive; only an unconfigured server falls back to same-origin.
  if (allowed.length) return allowed.includes(origin);
  return origin === `${req.protocol || (req.socket.encrypted ? 'https' : 'http')}://${req.headers.host}`;
}
module.exports = { JWT_OPTIONS, validateSecret, verifyToken, safePath, userWorkspace, originAllowed };
