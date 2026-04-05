/**
 * NovaComp Compiler Backend — Production Hardened
 *
 * Setup:
 *   1. Build binary:   g++ -o compiler compiler.cc lexer.cc inputbuf.cc parser2.cc
 *   2. Install deps:   npm install express cors express-rate-limit ws
 *   3. Start:          node server.js
 */

const express    = require("express");
const cors       = require("cors");
const { execFile, spawn } = require("child_process");
const fs         = require("fs");
const path       = require("path");
const os         = require("os");
const rateLimit  = require("express-rate-limit");
const http       = require("http");
const WebSocket  = require("ws");

const app    = express();
const PORT   = 3001;

const BINARY = path.resolve(
  __dirname,
  process.platform === "win32" ? "./compiler.exe" : "./compiler"
);

// ── Workspace (persistent user files) ────────────────────────────────────────
const WORKSPACE     = path.resolve(__dirname, "workspace");
const WORKSPACE_REL = "workspace"; // relative to __dirname for git paths
if (!fs.existsSync(WORKSPACE)) fs.mkdirSync(WORKSPACE, { recursive: true });

// ── Constants ─────────────────────────────────────────────────────────────────

const MAX_SOURCE_LENGTH  = 10_000;
const MAX_EXECUTION_MS   = 7_000;
const MAX_OUTPUT_BYTES   = 256_000;
const MAX_CONCURRENT     = 5;
const BENCH_ITERS        = 10000;
const REPL_IDLE_TIMEOUT  = 5 * 60 * 1000;  // 5 minutes

// ── State ─────────────────────────────────────────────────────────────────────

let activeExecutions = 0;

// ── Logging ───────────────────────────────────────────────────────────────────

function log(level, message, meta = {}) {
  const entry = {
    timestamp: new Date().toISOString(),
    level,
    message,
    ...meta,
  };
  console.log(JSON.stringify(entry));
}

// ── Middleware ────────────────────────────────────────────────────────────────

app.use(cors());
app.use(express.json({ limit: "64kb" }));

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

app.use("/run",     limiter);
app.use("/compare", limiter);

// ── Request Logging ───────────────────────────────────────────────────────────

app.use((req, _res, next) => {
  if (req.path !== "/health") {
    log("info", "Incoming request", {
      method: req.method,
      path:   req.path,
      ip:     req.ip,
    });
  }
  next();
});

// ── Stderr Filter ─────────────────────────────────────────────────────────────
// Strip internal debug lines that should never reach the user.
const DEBUG_PREFIXES = [
  "PASSIGN:", "DEBUG OUT:", "DEBUG:", "EXEC START:", "EXEC OUT:",
  "EXEC ASSIGN_F:", "IR:", "FOLDS:", "REMOVED:", "BENCH_US:",
  "BENCH_NODES:", "BENCH_ITERS:",
];

function filterStderr(raw) {
  if (!raw) return "";
  return raw
    .split("\n")
    .filter(line => {
      const t = line.trim();
      return t.length > 0 && !DEBUG_PREFIXES.some(p => t.startsWith(p));
    })
    .join("\n");
}

// ── Input Helpers ─────────────────────────────────────────────────────────────

function sanitizeSource(source) {
  source = source.replace(/\0/g, "");
  source = source.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  return source;
}

function validateSource(source) {
  if (!source || typeof source !== "string")
    return "Missing or invalid 'source' field.";
  if (source.trim().length === 0)
    return "Source code cannot be empty.";
  if (source.length > MAX_SOURCE_LENGTH)
    return `Source code too long. Maximum is ${MAX_SOURCE_LENGTH} characters.`;
  return null;
}

// ── Temp File Helpers ─────────────────────────────────────────────────────────

function writeTempFile(source) {
  const tmpDir  = fs.mkdtempSync(path.join(os.tmpdir(), "novacomp-"));
  const tmpFile = path.join(tmpDir, "program.csl");
  fs.writeFileSync(tmpFile, source + "\n", "utf8");
  return { tmpDir, tmpFile };
}

function cleanupTempFile(tmpFile, tmpDir) {
  try { fs.unlinkSync(tmpFile); } catch (_) {}
  try { fs.rmdirSync(tmpDir);   } catch (_) {}
}

// ── Run Helper ────────────────────────────────────────────────────────────────

function runCompiler(tmpFile, extraArgs = [], inputs = "") {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    let stdout = "";
    let stderr = "";

    const child = spawn(BINARY, [tmpFile, ...extraArgs], {
      stdio: ["pipe", "pipe", "pipe"]
    });

    child.stdout.on("data", (data) => { stdout += data.toString(); });
    child.stderr.on("data", (data) => { stderr += data.toString(); });

    if (inputs && inputs.trim().length > 0) {
      child.stdin.write(inputs.trim() + "\n");
    }
    child.stdin.end();

    const timer = setTimeout(() => {
      child.kill();
      reject({ type: "timeout", ms: Date.now() - start });
    }, MAX_EXECUTION_MS);

    child.on("close", (code) => {
      clearTimeout(timer);
      const ms = Date.now() - start;
      resolve({ stdout, stderr, ms, exitCode: code });
    });

    child.on("error", (err) => {
      clearTimeout(timer);
      reject({ type: "error", message: err.message });
    });
  });
}

// ── Bench Stats Parser ────────────────────────────────────────────────────────

function parseBenchStats(stdout, stderr) {
  const stats = {};
  for (const line of (stdout || "").split("\n")) {
    const parts = line.trim().split(":");
    if (parts.length >= 2) {
      const key = parts[0];
      const val = parts.slice(1).join(":");
      if (["BENCH_US", "BENCH_NODES", "BENCH_ITERS"].includes(key))
        stats[key] = parseFloat(val);
    }
  }
  for (const line of (stderr || "").split("\n")) {
    const parts = line.trim().split(":");
    if (parts.length >= 2) {
      const key = parts[0];
      const val = parts.slice(1).join(":");
      if (["FOLDS", "REMOVED"].includes(key))
        stats[key] = parseFloat(val);
    }
  }
  return {
    us:      stats["BENCH_US"]    ?? null,
    nodes:   stats["BENCH_NODES"] ?? null,
    iters:   stats["BENCH_ITERS"] ?? null,
    folds:   stats["FOLDS"]       ?? 0,
    removed: stats["REMOVED"]     ?? 0,
  };
}

// ── POST /run ─────────────────────────────────────────────────────────────────

app.post("/run", async (req, res) => {
  const validationError = validateSource(req.body?.source);
  if (validationError) {
    log("warn", "Invalid request", { reason: validationError, ip: req.ip });
    return res.status(400).json({ error: validationError });
  }

  if (activeExecutions >= MAX_CONCURRENT) {
    log("warn", "Concurrent limit reached", { activeExecutions, ip: req.ip });
    return res.status(503).json({ error: "Server is busy. Please try again in a moment." });
  }

  const source = sanitizeSource(req.body.source);
  const inputs = req.body.inputs || "";

  let tmpFile, tmpDir;
  try {
    ({ tmpFile, tmpDir } = writeTempFile(source));
  } catch (err) {
    log("error", "Failed to create temp file", { error: err.message });
    return res.status(500).json({ error: "Internal server error. Please try again." });
  }

  activeExecutions++;
  log("info", "Execution started", { activeExecutions, sourceLength: source.length, ip: req.ip });

  try {
    const { stdout, stderr, ms, exitCode } = await runCompiler(tmpFile, [], inputs);
    log("info", "Execution finished", { ms, exitCode, stdoutLength: stdout.length, activeExecutions });
    if (stdout && stdout.trim().length > 0)
      return res.json({ output: stdout, ms });
    const msg = filterStderr(stderr) || "Unknown runtime error.";
    return res.json({ error: msg, ms });
  } catch (e) {
    if (e.type === "timeout")
      return res.json({ error: "Execution timed out (> 7 seconds). Check for infinite loops.", ms: e.ms });
    return res.json({ error: "Unknown runtime error." });
  } finally {
    activeExecutions--;
    cleanupTempFile(tmpFile, tmpDir);
  }
});

// ── POST /compare ─────────────────────────────────────────────────────────────

app.post("/compare", async (req, res) => {
  const validationError = validateSource(req.body?.source);
  if (validationError) {
    log("warn", "Invalid compare request", { reason: validationError, ip: req.ip });
    return res.status(400).json({ error: validationError });
  }

  if (activeExecutions + 2 > MAX_CONCURRENT) {
    log("warn", "Concurrent limit reached for compare", { activeExecutions, ip: req.ip });
    return res.status(503).json({ error: "Server is busy. Please try again in a moment." });
  }

  const source = sanitizeSource(req.body.source);
  const inputs = req.body.inputs || "";

  let tmpFile, tmpDir;
  try {
    ({ tmpFile, tmpDir } = writeTempFile(source));
  } catch (err) {
    log("error", "Failed to create temp file for compare", { error: err.message });
    return res.status(500).json({ error: "Internal server error. Please try again." });
  }

  activeExecutions += 2;
  log("info", "Compare started", { activeExecutions, sourceLength: source.length, ip: req.ip });

  try {
    const normalRun      = await runCompiler(tmpFile, [], inputs);
    const normalBench    = await runCompiler(tmpFile, ["--benchmark", "--iters", String(BENCH_ITERS)], inputs);
    const optimizedRun   = await runCompiler(tmpFile, ["--optimize"], inputs);
    const optimizedBench = await runCompiler(tmpFile, ["--optimize", "--benchmark", "--iters", String(BENCH_ITERS)], inputs);

    const normalStats    = parseBenchStats(normalBench.stdout,    normalBench.stderr);
    const optimizedStats = parseBenchStats(optimizedBench.stdout, optimizedBench.stderr);
    console.log("NORMAL:", JSON.stringify(normalRun.stdout));
    console.log("OPTIMIZED:", JSON.stringify(optimizedRun.stdout));
    const normalize = s => s.trim().replace(/\r\n/g, '\n');
    const outputMismatch = normalize(normalRun.stdout) !== normalize(optimizedRun.stdout);

    log("info", "Compare finished", {
      normalUs: normalStats.us, optimizedUs: optimizedStats.us,
      normalNodes: normalStats.nodes, optNodes: optimizedStats.nodes,
    });

    return res.json({
      output: normalRun.stdout || "(no output)",
      outputMismatch,
      normal:    { us: normalStats.us, nodes: normalStats.nodes, iters: normalStats.iters },
      optimized: { us: optimizedStats.us, nodes: optimizedStats.nodes, iters: optimizedStats.iters, folds: optimizedStats.folds, removed: optimizedStats.removed },
      speedup: normalStats.us && optimizedStats.us ? (normalStats.us / optimizedStats.us).toFixed(2) : null,
      nodesEliminated: (normalStats.nodes ?? 0) - (optimizedStats.nodes ?? 0),
    });
  } catch (e) {
    if (e.type === "timeout")
      return res.json({ error: "Execution timed out (> 7 seconds). Check for infinite loops." });
    log("error", "Compare failed", { error: e.message });
    return res.json({ error: "Comparison failed. Please try again." });
  } finally {
    activeExecutions -= 2;
    cleanupTempFile(tmpFile, tmpDir);
  }
});

// ── Filesystem helpers ────────────────────────────────────────────────────────

function safeWorkspacePath(rel) {
  const resolved = path.resolve(WORKSPACE, rel || "");
  if (!resolved.startsWith(WORKSPACE + path.sep) && resolved !== WORKSPACE)
    return null;
  return resolved;
}

// ── POST /fs/write  { path, content } ────────────────────────────────────────
app.post("/fs/write", (req, res) => {
  const target = safeWorkspacePath(req.body?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, req.body?.content ?? "", "utf8");
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// ── DELETE /fs/delete?path=... ────────────────────────────────────────────────
app.delete("/fs/delete", (req, res) => {
  const target = safeWorkspacePath(req.query?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    fs.rmSync(target, { recursive: true, force: true });
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// ── POST /fs/mkdir  { path } ──────────────────────────────────────────────────
app.post("/fs/mkdir", (req, res) => {
  const target = safeWorkspacePath(req.body?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    fs.mkdirSync(target, { recursive: true });
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// ── POST /fs/rename  { from, to } ────────────────────────────────────────────
app.post("/fs/rename", (req, res) => {
  const src  = safeWorkspacePath(req.body?.from);
  const dest = safeWorkspacePath(req.body?.to);
  if (!src || !dest) return res.status(400).json({ error: "Invalid path" });
  try {
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.renameSync(src, dest);
    res.json({ ok: true });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// ── GET /git/status ───────────────────────────────────────────────────────────
app.get("/git/status", (_req, res) => {
  const { exec } = require("child_process");
  exec(`git status --porcelain -- ${WORKSPACE_REL}/`, { cwd: __dirname }, (err, stdout) => {
    if (err) return res.json({ status: {}, error: err.message });
    const status = {};
    const prefix = WORKSPACE_REL + "/";
    stdout.split("\n").forEach(line => {
      if (line.length < 4) return;
      const xy   = line.slice(0, 2);           // e.g. " M", "??", "R "
      let   file = line.slice(3).trim();        // e.g. "workspace/main.nova"
      // handle renames: "old -> new"
      if (file.includes(" -> ")) file = file.split(" -> ")[1];
      if (file.startsWith(prefix)) file = file.slice(prefix.length);
      const code = xy.trim() || xy[1];         // prefer non-space char
      status[file] = code === "?" ? "U" : code; // untracked shown as U like VS Code
    });
    res.json({ status });
  });
});

// ── GET /health ───────────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
  const binaryExists = fs.existsSync(BINARY);
  res.json({
    status:          binaryExists ? "ok" : "binary_missing",
    binary:          BINARY,
    exists:          binaryExists,
    activeExecutions,
    maxConcurrent:   MAX_CONCURRENT,
    uptime:          process.uptime(),
    replSessions:    replSessions.size,
  });
});

// ── Error Boundaries ──────────────────────────────────────────────────────────

app.use((err, req, res, _next) => {
  if (activeExecutions > 0) activeExecutions--;
  log("error", "Unhandled Express error", { error: err.message, stack: err.stack, path: req.path, ip: req.ip });
  res.status(500).json({ error: "Internal server error." });
});

process.on("unhandledRejection", (reason) => {
  log("error", "Unhandled promise rejection", { reason: reason?.toString() });
});

process.on("uncaughtException", (err) => {
  log("error", "Uncaught exception", { error: err.message, stack: err.stack });
  setTimeout(() => process.exit(1), 5000);
});

// ── WebSocket REPL ────────────────────────────────────────────────────────────
//
// Each connection gets its own isolated compiler --repl process.
// Messages flow:
//   browser → ws → child.stdin   (user types a line)
//   child.stdout → ws → browser  (REPL output)
//   child.stderr → ws → browser  (REPL errors)
//
// Protocol — messages are JSON:
//   browser sends:  { type: "input", line: "x = 5 ;" }
//                   { type: "ping" }
//   server sends:   { type: "output", text: ">>> " }
//                   { type: "output", text: "15\n" }
//                   { type: "error",  text: "Error at line 1: ..." }
//                   { type: "ready" }   ← sent once on connection
//                   { type: "closed" }  ← sent when process exits

const replSessions = new Map();  // ws → { child, idleTimer }

function createReplSession(ws) {
  log("info", "REPL session started", { activeSessions: replSessions.size + 1 });

  // spawn the compiler in REPL mode
  const child = spawn(BINARY, ["--repl"], {
    stdio: ["pipe", "pipe", "pipe"]
  });

  // ── helper: send JSON message to browser ──────────────────────────────────
  function send(type, text) {
    if (ws.readyState === WebSocket.OPEN)
      ws.send(JSON.stringify({ type, text }));
  }

  // ── idle timeout — kill session after 5 minutes of no input ──────────────
  let idleTimer = null;

  function resetIdleTimer() {
    if (idleTimer) clearTimeout(idleTimer);
    idleTimer = setTimeout(() => {
      log("info", "REPL session idle timeout");
      send("error", "\n  Session timed out after 5 minutes of inactivity.\n");
      send("closed", "");
      child.kill();
      ws.close();
    }, REPL_IDLE_TIMEOUT);
  }

  resetIdleTimer();

  // ── pipe child stdout to browser ──────────────────────────────────────────
  child.stdout.on("data", (data) => {
    send("output", data.toString());
  });

  // ── pipe child stderr to browser ─────────────────────────────────────────
  child.stderr.on("data", (data) => {
    send("error", data.toString());
  });

  // ── handle child exit ─────────────────────────────────────────────────────
  child.on("close", (code) => {
    log("info", "REPL process exited", { code });
    send("closed", "");
    if (idleTimer) clearTimeout(idleTimer);
    replSessions.delete(ws);
  });

  child.on("error", (err) => {
    log("error", "REPL process error", { error: err.message });
    send("error", `\n  Process error: ${err.message}\n`);
    send("closed", "");
    replSessions.delete(ws);
  });

  // ── store session ─────────────────────────────────────────────────────────
  replSessions.set(ws, { child, idleTimer: () => idleTimer });

  // ── notify browser that session is ready ─────────────────────────────────
  send("ready", "");
}

// ── Create HTTP server and attach WebSocket ───────────────────────────────────

const server = http.createServer(app);

const wss = new WebSocket.Server({ server, path: "/repl" });

wss.on("connection", (ws, req) => {
  log("info", "WebSocket REPL connection opened", {
    ip: req.socket.remoteAddress,
    activeSessions: replSessions.size + 1
  });

  createReplSession(ws);

  // ── handle messages from browser ─────────────────────────────────────────
  ws.on("message", (raw) => {
    let msg;
    try { msg = JSON.parse(raw); }
    catch { return; }

    const session = replSessions.get(ws);
    if (!session) return;

    if (msg.type === "input") {
      // reset idle timer on every input
      if (session.idleTimer) clearTimeout(session.idleTimer());
      session._idleTimer = setTimeout(() => {
        log("info", "REPL session idle timeout");
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: "error", text: "\n  Session timed out after 5 minutes of inactivity.\n" }));
          ws.send(JSON.stringify({ type: "closed", text: "" }));
        }
        session.child.kill();
        ws.close();
      }, REPL_IDLE_TIMEOUT);

      // send line to REPL process stdin
      const line = (msg.line || "") + "\n";
      try { session.child.stdin.write(line); } catch (_) {}
    }
    else if (msg.type === "ping") {
      // keepalive — just reset idle timer, no response needed
    }
  });

  // ── handle browser disconnect ─────────────────────────────────────────────
  ws.on("close", () => {
    log("info", "WebSocket REPL connection closed");
    const session = replSessions.get(ws);
    if (session) {
      try { session.child.kill(); } catch (_) {}
      replSessions.delete(ws);
    }
  });

  ws.on("error", (err) => {
    log("error", "WebSocket error", { error: err.message });
  });
});

// ── Start (use server.listen instead of app.listen for WebSocket support) ────

server.listen(PORT, () => {
  log("info", "NovaComp server started", {
    port:            PORT,
    binary:          BINARY,
    binaryFound:     fs.existsSync(BINARY),
    maxConcurrent:   MAX_CONCURRENT,
    maxSourceLength: MAX_SOURCE_LENGTH,
    maxExecutionMs:  MAX_EXECUTION_MS,
    benchIters:      BENCH_ITERS,
    replIdleTimeout: REPL_IDLE_TIMEOUT,
  });

  if (!fs.existsSync(BINARY)) {
    log("warn", "Binary not found", {
      path: BINARY,
      fix:  "g++ -o compiler compiler.cc lexer.cc inputbuf.cc parser2.cc",
    });
  }
});