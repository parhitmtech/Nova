'use strict';
// The Express app: middleware, rate limits, auth gates and routers, in the order requests see them.
const express    = require("express");
const cors       = require("cors");
const fs         = require("fs");
const path       = require("path");
const rateLimit  = require("express-rate-limit");
const security   = require("./services/security");
const paths      = require("./paths");
const { log } = require("./lib/logger");
const { requireAuth } = require("./middleware/auth");
const { TRUST_PROXY } = require("./config");

const app = express();
// Rate limits and origin checks see the real client only behind an explicitly trusted proxy.
app.set('trust proxy', TRUST_PROXY);

// ── Middleware ────────────────────────────────────────────────────────────────

app.use((req, res, next) => {
  if (!security.originAllowed(req.headers.origin, req)) return res.status(403).json({ error: 'Origin not allowed' });
  next();
});
app.use(cors({ origin: true }));
app.use(express.json({ limit: "64kb" }));

app.get("/", (req, res) => {
  res.sendFile(path.join(paths.PUBLIC_DIR, "landing.html"));
});

app.use(express.static(paths.STATIC_DIR));

// ── Rate Limiting ─────────────────────────────────────────────────────────────

const limiter = rateLimit({
  windowMs: 60 * 1000,
  max: 30,
  standardHeaders: true,
  legacyHeaders: false,
  handler: (req, res) => {
    log("warn", "Rate limit exceeded", { ip: req.ip });
    res.status(429).json({
      error: "Too many requests. Please wait a moment before running again."
    });
  },
});

const agentLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 20,
  handler: (_req, res) => {
    res.status(429).json({ error: 'Too many messages. Please wait a moment.' });
  },
});

app.use('/auth', rateLimit({ windowMs: 60000, max: 10, standardHeaders: true, legacyHeaders: false }));
app.use(['/run', '/run-stream', '/compile-bytecode', '/compare', '/fs', '/gpu', '/git', '/agent'], requireAuth);
app.use(['/run-stream', '/compile-bytecode', '/gpu/submit', '/api/share'], limiter);
app.use('/api/share', (req, res, next) => req.method === 'POST' ? requireAuth(req, res, next) : next());
app.use('/fs', rateLimit({ windowMs: 60000, max: 120, keyGenerator: req => req.user.userId, standardHeaders: true, legacyHeaders: false }));
function gpuEnabled() { return process.env.NOVA_ENABLE_GPU === 'true' && process.env.NOVA_EXECUTION_MODE === 'local' && process.env.NODE_ENV !== 'production'; }
// The IDE hides actions this deployment cannot perform.
app.get('/api/capabilities', requireAuth, (_req, res) => res.json({ gpu: gpuEnabled(), ml: false }));
app.use('/gpu', (req, res, next) => {
  if (!gpuEnabled()) return res.status(503).json({ error: 'GPU execution is disabled on this deployment' });
  next();
});
app.use('/run-debug', requireAuth, (_req, res) => res.status(501).json({ error: 'Debug sessions require isolated session support and are unavailable' }));
app.use("/run",     limiter);
app.use("/compare", limiter);
app.use('/agent/chat', agentLimiter);

// ── Request Logging ───────────────────────────────────────────────────────────

// Paths that are too chatty to log on every call (filesystem sync, health checks)
const SILENT_PATHS = new Set(["/health", "/fs/write", "/fs/mkdir", "/fs/read", "/git/status"]);

app.use((req, _res, next) => {
  const silent = SILENT_PATHS.has(req.path) || req.path.startsWith('/gpu/status/');
  if (!silent) {
    log("info", "Incoming request", {
      method: req.method,
      path:   req.path,
      ip:     req.ip,
    });
  }
  next();
});

// ── Routes ────────────────────────────────────────────────────────────────────

app.use(require("./routes/auth"));
app.use(require("./routes/run"));
app.use(require("./routes/fs"));
app.use(require("./routes/health"));
app.use(require("./routes/files"));
app.use(require("./routes/gpu"));
app.use(require("./routes/share"));
app.use(require("./routes/agent"));

// ── SPA fallback — serve React app for all non-API routes ────────────────────
app.use((req, res) => {
  const index = path.join(paths.STATIC_DIR, 'index.html');
  if (!fs.existsSync(index)) return res.status(404).json({ error: 'Not found' });
  res.sendFile(index);
});

// ── Error Boundary (last, so it also covers the share and agent routes) ───────

app.use((err, req, res, _next) => {
  if (res.headersSent) return res.end();
  if (err.type === "entity.too.large") return res.status(413).json({ error: "Request body exceeds 64KB" });
  log("error", "Unhandled Express error", { error: err.message, stack: err.stack, path: req.path, ip: req.ip });
  res.status(500).json({ error: "Internal server error." });
});

module.exports = app;
