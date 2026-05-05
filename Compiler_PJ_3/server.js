/**
 * NovaComp Compiler Backend — Production Hardened
 *
 * Setup:
 *   1. Build binary:   g++ -o compiler compiler.cc lexer.cc inputbuf.cc parser2.cc
 *   2. Install deps:   npm install express cors express-rate-limit ws dotenv bcryptjs jsonwebtoken @aws-sdk/client-dynamodb @aws-sdk/lib-dynamodb @aws-sdk/client-s3
 *   3. Start:          node server.js
 */

require("dotenv").config();

const express    = require("express");
const cors       = require("cors");
const crypto     = require("crypto");
const { execFile, spawn } = require("child_process");
const fs         = require("fs");
const path       = require("path");
const os         = require("os");
const rateLimit  = require("express-rate-limit");
const http       = require("http");
const WebSocket  = require("ws");
const { types }  = require("util");
const { debug }  = require("console");
const bcrypt     = require("bcryptjs");
const jwt        = require("jsonwebtoken");

const { DynamoDBClient, CreateTableCommand, DescribeTableCommand } = require("@aws-sdk/client-dynamodb");
const { DynamoDBDocumentClient, PutCommand, GetCommand, DeleteCommand } = require("@aws-sdk/lib-dynamodb");
const { S3Client, PutObjectCommand, GetObjectCommand, DeleteObjectCommand, ListObjectsV2Command } = require("@aws-sdk/client-s3");

// node-pty is optional — install with: npm install node-pty
// Required for the Terminal tab in the web IDE.
let pty = null;
try { pty = require('node-pty'); } catch (_) {}

const app    = express();
const PORT   = parseInt(process.env.PORT || "3001", 10);

// ── AWS Clients ───────────────────────────────────────────────────────────────

const AWS_REGION  = process.env.AWS_REGION        || "us-east-1";
const S3_BUCKET   = process.env.S3_BUCKET         || "novacomp-files";
const USERS_TABLE = process.env.DYNAMODB_TABLE_USERS || "novacomp-users";
const FILES_TABLE = "novacomp-files-meta";
const JWT_SECRET  = process.env.JWT_SECRET         || "novacomp-secret-key";

const dynamoRaw = new DynamoDBClient({ region: AWS_REGION });
const dynamo    = DynamoDBDocumentClient.from(dynamoRaw);
const s3        = new S3Client({ region: AWS_REGION });

// ── DynamoDB Table Bootstrap ──────────────────────────────────────────────────

async function ensureTables() {
  const tables = [
    {
      TableName: USERS_TABLE,
      KeySchema: [{ AttributeName: "email", KeyType: "HASH" }],
      AttributeDefinitions: [{ AttributeName: "email", AttributeType: "S" }],
      BillingMode: "PAY_PER_REQUEST",
    },
    {
      TableName: FILES_TABLE,
      KeySchema: [
        { AttributeName: "userId",   KeyType: "HASH" },
        { AttributeName: "filePath", KeyType: "RANGE" },
      ],
      AttributeDefinitions: [
        { AttributeName: "userId",   AttributeType: "S" },
        { AttributeName: "filePath", AttributeType: "S" },
      ],
      BillingMode: "PAY_PER_REQUEST",
    },
  ];

  for (const params of tables) {
    try {
      await dynamoRaw.send(new DescribeTableCommand({ TableName: params.TableName }));
      log("info", "DynamoDB table already exists", { table: params.TableName });
    } catch (e) {
      if (e.name === "ResourceNotFoundException") {
        await dynamoRaw.send(new CreateTableCommand(params));
        log("info", "DynamoDB table created", { table: params.TableName });
      } else {
        log("error", "DynamoDB DescribeTable failed", { table: params.TableName, error: e.message });
      }
    }
  }
}

// ── JWT Auth Middleware ───────────────────────────────────────────────────────

function requireAuth(req, res, next) {
  const token = req.headers.authorization?.split(" ")[1];
  if (!token) return res.status(401).json({ error: "Unauthorized" });
  try {
    req.user = jwt.verify(token, JWT_SECRET);
    next();
  } catch {
    res.status(401).json({ error: "Invalid token" });
  }
}

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
const MAX_EXECUTION_MS   = 30_000;          // default: 30s (infinite loops caught by compiler)
const ML_EXECUTION_MS    = 10 * 60 * 1000; // @ml annotation: 10 minutes
const MAX_OUTPUT_BYTES   = 256_000;
const MAX_CONCURRENT     = 5;
const BENCH_ITERS        = 10000;
const REPL_IDLE_TIMEOUT  = 5 * 60 * 1000;  // 5 minutes

// Returns the appropriate timeout for a given source.
// Add "// @ml" anywhere in the file to opt into the ML timeout.
function getExecutionTimeout(source) {
  return /\/\/\s*@ml\b/.test(source) ? ML_EXECUTION_MS : MAX_EXECUTION_MS;
}

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
app.use(express.json({ limit: "50mb" }));

app.get("/", (req, res) => {
  res.sendFile(path.join(__dirname, "landing.html"));
});

app.use(express.static(path.join(__dirname, 'dist')));
app.use(express.static(__dirname));
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

// Paths that are too chatty to log on every call (filesystem sync, health checks)
const SILENT_PATHS = new Set(["/health", "/fs/write", "/fs/mkdir", "/fs/read", "/git/status"]);

app.use((req, _res, next) => {
  if (!SILENT_PATHS.has(req.path)) {
    log("info", "Incoming request", {
      method: req.method,
      path:   req.path,
      ip:     req.ip,
    });
  }
  next();
});

// ── Auth Endpoints ────────────────────────────────────────────────────────────

// POST /auth/signup  { email, password }
app.post("/auth/signup", async (req, res) => {
  const { email, password } = req.body || {};
  if (!email || !password)
    return res.status(400).json({ error: "email and password required" });

  try {
    const existing = await dynamo.send(new GetCommand({ TableName: USERS_TABLE, Key: { email } }));
    if (existing.Item) return res.status(409).json({ error: "Email already registered" });

    const userId       = crypto.randomUUID();
    const passwordHash = await bcrypt.hash(password, 10);
    const createdAt    = new Date().toISOString();

    await dynamo.send(new PutCommand({
      TableName: USERS_TABLE,
      Item: { email, userId, passwordHash, createdAt },
    }));

    const token = jwt.sign({ userId, email }, JWT_SECRET, { expiresIn: "7d" });
    res.json({ token, user: { userId, email, createdAt } });
  } catch (e) {
    log("error", "Signup failed", { error: e.message });
    res.status(500).json({ error: "Signup failed" });
  }
});

// POST /auth/login  { email, password }
app.post("/auth/login", async (req, res) => {
  const { email, password } = req.body || {};
  if (!email || !password)
    return res.status(400).json({ error: "email and password required" });

  try {
    const result = await dynamo.send(new GetCommand({ TableName: USERS_TABLE, Key: { email } }));
    if (!result.Item) return res.status(401).json({ error: "Invalid credentials" });

    const valid = await bcrypt.compare(password, result.Item.passwordHash);
    if (!valid) return res.status(401).json({ error: "Invalid credentials" });

    const { userId, createdAt } = result.Item;
    const token = jwt.sign({ userId, email }, JWT_SECRET, { expiresIn: "7d" });
    res.json({ token, user: { userId, email, createdAt } });
  } catch (e) {
    log("error", "Login failed", { error: e.message });
    res.status(500).json({ error: "Login failed" });
  }
});

// GET /auth/me  (requires Bearer token)
app.get("/auth/me", requireAuth, (req, res) => {
  res.json({ user: req.user });
});

// ── Stderr Filter ─────────────────────────────────────────────────────────────
// Strip internal debug lines that should never reach the user.
const DEBUG_PREFIXES = [
  "PASSIGN:", "DEBUG OUT:", "DEBUG:", "EXEC START:", "EXEC OUT:",
  "EXEC ASSIGN_F:", "IR:", "FOLDS:", "REMOVED:", "BENCH_US:",
  "BENCH_NODES:", "BENCH_ITERS:",
  "NOVA_HOME=",
  "ASSIGN_D",
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

// Use a local _tmp folder right next to server.js — short path, no permission issues
const LOCAL_TEMP = path.join(__dirname, '_tmp');
if (!fs.existsSync(LOCAL_TEMP)) fs.mkdirSync(LOCAL_TEMP, { recursive: true });

function writeTempFile(source) {
  const id = Date.now() + '_' + Math.random().toString(36).slice(2);
  const tmpDir  = path.join(LOCAL_TEMP, id);
  const tmpFile = path.join(tmpDir, 'program.csl');
  fs.mkdirSync(tmpDir, { recursive: true });
  fs.writeFileSync(tmpFile, source + '\n', 'utf8');
  return { tmpDir, tmpFile };
}

function cleanupTempFile(tmpFile, tmpDir) {
  // try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (_) {}
}
// ── Run Helper ────────────────────────────────────────────────────────────────

function runCompiler(tmpFile, extraArgs = [], inputs = "", timeoutMs = MAX_EXECUTION_MS) {
  console.log(`\n🚀 runCompiler START`);
  console.log(`   tmpFile: ${tmpFile}`);
  console.log(`   timeout: ${timeoutMs}ms (${timeoutMs/1000}s)`);
  console.log(`   time: ${new Date().toISOString()}`);
  
  return new Promise((resolve, reject) => {
    const start = Date.now();
    let stdout = "";
    let stderr = "";

    const child = spawn(BINARY, [tmpFile, ...extraArgs], {
      stdio: ["pipe", "pipe", "pipe"],
      cwd: WORKSPACE,
      shell: process.platform === 'win32'
    });

    child.stdout.on("data", (data) => { 
      stdout += data.toString(); 
      console.log(`📤 stdout chunk: ${data.toString().substring(0, 50)}...`);
    });
    
    child.stderr.on("data", (data) => { 
      stderr += data.toString();
      console.log(`📤 stderr chunk: ${data.toString().substring(0, 50)}...`);
    });

    if (inputs && inputs.trim().length > 0) {
      child.stdin.write(inputs.trim() + "\n");
    }
    child.stdin.end();

    const timer = setTimeout(() => {
      const elapsed = Date.now() - start;
      console.log(`⏰ TIMEOUT TRIGGERED after ${elapsed}ms`);
      child.kill();
      reject({ type: "timeout", ms: elapsed });
    }, timeoutMs);

    child.on("close", (code) => {
      clearTimeout(timer);
      const ms = Date.now() - start;
      console.log(`✅ Process closed: code=${code}, time=${ms}ms`);
      resolve({ stdout, stderr, ms, exitCode: code });
    });

    child.on("error", (err) => {
      clearTimeout(timer);
      console.log(`❌ Process error: ${err.message}`);
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

// POST /run 

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
  
  const timeout = getExecutionTimeout(source);
  log("info", "Execution started", { activeExecutions, sourceLength: source.length, timeout, ip: req.ip });

  try {
    const { stdout, stderr, ms, exitCode } = await runCompiler(tmpFile, [], inputs, timeout);
    log("info", "Execution finished", { ms, exitCode, stdoutLength: stdout.length, activeExecutions });
    const terminalOutput = filterStderr(stderr) || "";
    if (stdout && stdout.trim().length > 0)
      return res.json({ output: stdout, terminal: terminalOutput, ms })
    const msg = terminalOutput || "Unknown runtime error.";
    return res.json({ error: msg, terminal: terminalOutput, ms })
  } catch (e) {
    if (e.type === "timeout")
      return res.json({ error: "Execution timed out. Check for infinite loops, or add // @ml for a 10-minute limit.", ms: e.ms });
    return res.json({ error: "Unknown runtime error." });
  } finally {
    activeExecutions--;
    cleanupTempFile(tmpFile, tmpDir);
  }
});

app.post("/run-stream", (req, res) => {
    const validationError = validateSource(req.body?.source);
    if (validationError) return res.status(400).json({ error: validationError });
    if (activeExecutions >= MAX_CONCURRENT) return res.status(503).json({ error: "Server busy." });

    const source = sanitizeSource(req.body.source);
    const inputs = req.body.inputs || "";
    let tmpFile, tmpDir;
    try { ({ tmpFile, tmpDir } = writeTempFile(source)); }
    catch (err) { return res.status(500).json({ error: "Internal error." }); }

    res.setHeader('Content-Type', 'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.setHeader('Connection', 'keep-alive');
    res.flushHeaders();

    activeExecutions++;
    const start = Date.now();
    const send = (type, text) => res.write(`data: ${JSON.stringify({ type, text })}\n\n`);
    const execTimeout = getExecutionTimeout(source);

    let timedOut = false;
    let stdout = '';
    let stderr = '';

    const child = spawn(BINARY, [tmpFile], {
        stdio: ['pipe', 'pipe', 'pipe'],
        cwd: WORKSPACE,
        shell: false,
        windowsHide: true,
        env: { ...process.env, NOVA_HOME: __dirname }
    });

    let stdoutBuf = '';
    let flushTimer = null;

    // Stream stdout in real-time
    child.stdout.on('data', d => {
        stdoutBuf += d.toString();
        if (stdoutBuf.includes('\n')) {
            clearTimeout(flushTimer);
            send('stdout', stdoutBuf);
            stdoutBuf = '';
        } else {
            clearTimeout(flushTimer);
            flushTimer = setTimeout(() => {
                if (stdoutBuf) { send('stdout', stdoutBuf); stdoutBuf = ''; }
            }, 50);
        }
    });

    child.stderr.on('data', d => {
        const text = d.toString();
        stderr += text;
    });

    // Write inputs to stdin
    if (inputs.trim()) child.stdin.write(inputs.trim() + '\n');
    child.stdin.end();

    // Manual timeout
    const timer = setTimeout(() => {
        timedOut = true;
        child.kill();
    }, execTimeout);

    child.on('close', (code) => {
        clearTimeout(timer);
        if (stdoutBuf) { send('stdout', stdoutBuf); stdoutBuf = ''; }
        const f = filterStderr(stderr);
        if (f) send('stderr', f);
        if (timedOut) {
            const secs = Math.round(execTimeout / 1000);
            send('error', `Execution timed out (> ${secs}s). Add // @ml for a 10-minute limit.`);
        } else if (code !== 0 && !stdout) {
            send('error', `Runtime error (exit code ${code})`);
        }
        send('done', String(Date.now() - start));
        res.end();
        activeExecutions--;
        cleanupTempFile(tmpFile, tmpDir);
    });

    child.on('error', err => {
        clearTimeout(timer);
        send('error', err.message);
        send('done', String(Date.now() - start));
        res.end();
        activeExecutions--;
        cleanupTempFile(tmpFile, tmpDir);
    });
});

// Debug session state
let debugChild = null;
let debugRes = null;

app.post("/run-debug/start", (req, res) => {
    const validationError = validateSource(req.body?.source);
    if (validationError) return res.status(400).json({ error: validationError });

    let tmpFile, tmpDir;
    try { ({ tmpFile, tmpDir } = writeTempFile(sanitizeSource(req.body.source))); }
    catch (err) { return res.status(500).json({ error: "Internal error." }); }

    // Kill any existing debug session
    if (debugChild) { try { debugChild.kill(); } catch(_) {} debugChild = null; }

    res.setHeader('Content-Type', 'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.setHeader('Connection', 'keep-alive');
    res.flushHeaders();
    debugRes = res;

    const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

    // Use spawn — we need interactive stdin for step/continue commands
    debugChild = spawn(BINARY, ['--debug', tmpFile], {
        stdio: ['pipe', 'pipe', 'pipe'],
        cwd: WORKSPACE,
        shell: false,
        windowsHide: true,
        env: { ...process.env, NOVA_HOME: __dirname }
    });

    // ADD THESE TEMPORARILY:
    console.log('DEBUG: spawned PID', debugChild.pid);
    debugChild.on('error', err => console.log('DEBUG spawn error:', err.message));
    debugChild.on('close', (code, signal) => console.log('DEBUG close code:', code, signal));

    debugChild.on('error', err => {
        send({ type: 'error', text: err.message });
        send({ type: 'done' });
        res.end();
        debugChild = null;
    });

    // Stream stdout line by line
    let buf = '';
    debugChild.stdout.on('data', d => {
        buf += d.toString();
        let nl;
        while ((nl = buf.indexOf('\n')) !== -1) {
            const line = buf.slice(0, nl).trimEnd();
            buf = buf.slice(nl + 1);
            if (line.startsWith('__DEBUG__')) {
                try {
                    const obj = JSON.parse(line.slice(9));
                    send({ type: 'debug', line: obj.line, vars: obj.vars || {} });
                } catch(_) {}
            } else if (line.length > 0) {
                send({ type: 'stdout', text: line + '\n' });
            }
        }
    });

    debugChild.stderr.on('data', d => {
        const f = filterStderr(d.toString());
        if (f) send({ type: 'stderr', text: f });
    });

    debugChild.on('close', () => {
        send({ type: 'done' });
        res.end();
        debugChild = null;
        cleanupTempFile(tmpFile, tmpDir);
    });

    // req.on('close', () => {
    //   // Small delay - SSe connections can briefly disconnecting on start
    //   setTimeout(() => {
    //     if (debugChild) {
    //       try { debugChild.kill(); } catch(_) {}
    //       debugChild = null;
    //     }
    //   }, 500);
    // });
});

app.post("/run-debug/command", (req, res) => {
  const cmd = req.body?.command;
  if (!debugChild || !['step', 'continue', 'stop'].includes(cmd)) {
    return res.json({ ok: false });
  }
  try {
    debugChild.stdin.write(cmd + '\n');
    res.json({ ok: true });
  } catch (e) {
    res.json({ ok: false, error: e.message });
  }
});

app.post("/run-debug/breakpoints", (req, res) => {
  const lines = req.body?.lines || [];
  if (debugChild)
  {
    debugChild.stdin.write('breakpoints:' + lines.join(',') + '\n');
  }
  res.json({ ok: true });
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

// ── GET /fs/read?path=... ─────────────────────────────────────────────────────
app.get("/fs/read", (req, res) => {
  const target = safeWorkspacePath(req.query?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    const content = fs.readFileSync(target, "utf8");
    res.json({ content });
  } catch (e) {
    res.status(404).json({ error: e.message });
  }
});

// ── GET /fs/tree ──────────────────────────────────────────────────────────────
app.get("/fs/tree", (req, res) => {
  const walk = (dir, prefix = '') => {
    const items = [];
    try {
      const entries = fs.readdirSync(dir, { withFileTypes: true });
      
      for (const entry of entries) {
        // Skip hidden files and node_modules
        if (entry.name.startsWith('.') || entry.name === 'node_modules') continue;
        
        const relPath = prefix ? `${prefix}/${entry.name}` : entry.name;
        
        items.push({
          path: relPath,
          name: entry.name,
          isFolder: entry.isDirectory()
        });
        
        // Recursively walk subdirectories
        if (entry.isDirectory()) {
          const fullPath = path.join(dir, entry.name);
          items.push(...walk(fullPath, relPath));
        }
      }
    } catch (err) {
      log("error", "Failed to read directory", { dir, error: err.message });
    }
    
    return items;
  };
  
  try {
    const files = walk(WORKSPACE);
    res.json({ files });
  } catch (err) {
    log("error", "Failed to walk workspace", { error: err.message });
    res.status(500).json({ error: err.message });
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
    ptyLoaded:       !!pty,
    terminalShell:   resolveShell().shell,
  });
});

// ── S3 File Endpoints (auth-protected) ───────────────────────────────────────

// GET /files — list user's files (S3 userId/ prefix)
app.get("/files", requireAuth, async (req, res) => {
  try {
    const { Contents = [] } = await s3.send(new ListObjectsV2Command({
      Bucket: S3_BUCKET,
      Prefix: `${req.user.userId}/`,
    }));
    const files = Contents.map(obj => ({
      fileName:  obj.Key.replace(`${req.user.userId}/`, ""),
      updatedAt: obj.LastModified,
    }));
    res.json({ files });
  } catch (e) {
    log("error", "Failed to list files", { error: e.message });
    res.status(500).json({ error: "Failed to list files" });
  }
});

// POST /files/save  { fileName, content }
app.post("/files/save", requireAuth, async (req, res) => {
  const { fileName, content } = req.body || {};
  if (!fileName || content === undefined)
    return res.status(400).json({ error: "fileName and content required" });

  const safeName  = path.basename(fileName).replace(/[^a-zA-Z0-9._-]/g, "_");
  const key       = `${req.user.userId}/${safeName}`;
  const updatedAt = new Date().toISOString();

  try {
    await s3.send(new PutObjectCommand({
      Bucket:      S3_BUCKET,
      Key:         key,
      Body:        content,
      ContentType: "text/plain",
    }));
    await dynamo.send(new PutCommand({
      TableName: FILES_TABLE,
      Item: { userId: req.user.userId, filePath: safeName, fileName: safeName, updatedAt },
    }));
    res.json({ ok: true, fileName: safeName, updatedAt });
  } catch (e) {
    log("error", "Failed to save file", { error: e.message });
    res.status(500).json({ error: "Failed to save file" });
  }
});

// GET /files/load?fileName=x
app.get("/files/load", requireAuth, async (req, res) => {
  const fileName = req.query.fileName;
  if (!fileName) return res.status(400).json({ error: "fileName required" });

  const safeName = path.basename(fileName).replace(/[^a-zA-Z0-9._-]/g, "_");
  const key      = `${req.user.userId}/${safeName}`;

  try {
    const { Body } = await s3.send(new GetObjectCommand({ Bucket: S3_BUCKET, Key: key }));
    const content  = await Body.transformToString();
    res.json({ content, fileName: safeName });
  } catch (e) {
    if (e.name === "NoSuchKey") return res.status(404).json({ error: "File not found" });
    log("error", "Failed to load file", { error: e.message });
    res.status(500).json({ error: "Failed to load file" });
  }
});

// DELETE /files/delete?fileName=x
app.delete("/files/delete", requireAuth, async (req, res) => {
  const fileName = req.query.fileName;
  if (!fileName) return res.status(400).json({ error: "fileName required" });

  const safeName = path.basename(fileName).replace(/[^a-zA-Z0-9._-]/g, "_");
  const key      = `${req.user.userId}/${safeName}`;

  try {
    await s3.send(new DeleteObjectCommand({ Bucket: S3_BUCKET, Key: key }));
    await dynamo.send(new DeleteCommand({
      TableName: FILES_TABLE,
      Key: { userId: req.user.userId, filePath: safeName },
    }));
    res.json({ ok: true });
  } catch (e) {
    log("error", "Failed to delete file", { error: e.message });
    res.status(500).json({ error: "Failed to delete file" });
  }
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
    stdio: ["pipe", "pipe", "pipe"],
    env: { ...process.env, NOVA_HOME: __dirname }
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

// ── Shell resolver (for terminal) ─────────────────────────────────────────────

function resolveShell() {
  if (process.env.NOVA_SHELL) {
    const args = process.env.NOVA_SHELL_ARGS ? process.env.NOVA_SHELL_ARGS.split(',') : [];
    return { shell: process.env.NOVA_SHELL, args };
  }
  if (process.platform === 'win32') {
    // Default to PowerShell — always available on Windows 10/11.
    // To use WSL: set NOVA_SHELL=wsl.exe  NOVA_SHELL_ARGS=bash,-l
    return { shell: 'powershell.exe', args: [] };
  }
  return { shell: process.env.SHELL || '/bin/bash', args: [] };
}

// ── Create HTTP server ────────────────────────────────────────────────────────

const server = http.createServer(app);

// ── Single WebSocket server — routes by path ──────────────────────────────────
// Using noServer + manual upgrade routing avoids the conflict that occurs when
// two WebSocket.Server instances both attach upgrade listeners to the same
// http.Server (they can intercept each other's connections).

const wss     = new WebSocket.Server({ noServer: true });  // REPL
const termWss = new WebSocket.Server({ noServer: true });  // Terminal

server.on('upgrade', (req, socket, head) => {
  const url      = new URL(req.url, 'http://localhost');
  const pathname = url.pathname;
  if (pathname === '/repl') {
    wss.handleUpgrade(req, socket, head, ws => wss.emit('connection', ws, req));
  } else if (pathname === '/terminal') {
    try {
      const payload = jwt.verify(url.searchParams.get('token') || '', JWT_SECRET);
      req._userId = payload.userId;
    } catch { req._userId = null; }
    termWss.handleUpgrade(req, socket, head, ws => termWss.emit('connection', ws, req));
  } else {
    socket.destroy();
  }
});

// ── REPL connection handler ───────────────────────────────────────────────────

wss.on("connection", (ws, req) => {
  log("info", "WebSocket REPL connection opened", {
    ip: req.socket.remoteAddress,
    activeSessions: replSessions.size + 1
  });

  createReplSession(ws);

  ws.on("message", (raw) => {
    let msg;
    try { msg = JSON.parse(raw); }
    catch { return; }

    const session = replSessions.get(ws);
    if (!session) return;

    if (msg.type === "input") {
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

      const line = (msg.line || "") + "\n";
      try { session.child.stdin.write(line); } catch (_) {}
    }
  });

  ws.on("close", () => {
    log("info", "WebSocket REPL connection closed");
    const session = replSessions.get(ws);
    if (session) {
      try { session.child.kill(); } catch (_) {}
      replSessions.delete(ws);
    }
  });

  ws.on("error", (err) => {
    log("error", "WebSocket REPL error", { error: err.message });
  });
});

// ── Sandboxed Workspace Shell ─────────────────────────────────────────────────
// A custom shell that runs entirely inside the IDE's workspace/ directory.
// It cannot access anything outside that folder — no real shell is spawned.
// Supports: ls, cd, pwd, cat, mkdir, rm, cp, mv, touch, echo, novacomp, clear, help

async function createShellSession(ws, userId) {
  const userWorkspace = userId
    ? path.join(WORKSPACE, userId.replace(/[^a-zA-Z0-9-]/g, ''))
    : WORKSPACE;
  fs.mkdirSync(userWorkspace, { recursive: true });

  log('info', 'Shell session started', { userId: userId || 'anonymous' });

  let cwd        = userWorkspace;   // current directory (absolute)
  let inputBuf   = '';           // typed-but-not-submitted characters
  let history    = [];           // command history
  let histIdx    = -1;           // -1 = not navigating
  let currentProc = null;        // currently running child process (for Ctrl+C)
  let histDraft = '';          // saved draft when navigating up

  // ── helpers ───────────────────────────────────────────────────────────────

  function send(text) {
    if (ws.readyState === WebSocket.OPEN) ws.send(text);
  }

  // Send a JSON control message to the browser to sync filesystem state
  function notifyFsChange(op, paths) {
    if (ws.readyState === WebSocket.OPEN)
      ws.send(JSON.stringify({ __nc: 'fs', op, paths }));
  }
  function rel(p) { return path.relative(userWorkspace, p).replace(/\\/g, '/'); }

  function prompt() {
    const rel = path.relative(userWorkspace, cwd).replace(/\\/g, '/');
    return `\x1b[32mnovacomp\x1b[0m:\x1b[34m${rel ? '~/' + rel : '~'}\x1b[0m$ `;
  }

  function showPrompt() { send('\r\n' + prompt()); }

  // Replace the current input line on screen with newInput
  function replaceInput(newInput) {
    if (inputBuf.length > 0) send(`\x1b[${inputBuf.length}D\x1b[K`);
    inputBuf = newInput;
    send(inputBuf);
  }

  // Resolve a user-supplied path safely — returns null if outside workspace
  function safe(p) {
    if (!p || p === '~') return WORKSPACE;
    const abs = path.isAbsolute(p)
      ? path.join(userWorkspace, p)      // treat /foo as ~/foo
      : path.resolve(cwd, p);
    const norm = path.resolve(abs);
    if (norm !== userWorkspace && !norm.startsWith(userWorkspace + path.sep)) return null;
    return norm;
  }

  // ── commands ──────────────────────────────────────────────────────────────

  function cmdLs(args) {
    const long    = args.some(a => /^-\w*l/.test(a));
    const all     = args.some(a => /^-\w*a/.test(a));
    const targets = args.filter(a => !a.startsWith('-'));
    const dir     = safe(targets[0] || '.');
    if (!dir)               { send("\x1b[31mls: permission denied\x1b[0m"); return; }
    if (!fs.existsSync(dir)){ send(`\x1b[31mls: cannot access '${targets[0]}': No such file or directory\x1b[0m`); return; }
    try {
      const entries = fs.readdirSync(dir).filter(e => all || !e.startsWith('.'));
      if (!entries.length) return;
      if (long) {
        send(entries.map(name => {
          const st  = fs.statSync(path.join(dir, name));
          const dir2 = st.isDirectory();
          return `${dir2 ? 'd' : '-'}rw-r--r--  ${String(dir2 ? '-' : st.size).padStart(8)}  `
               + `${st.mtime.toISOString().slice(0,10)}  `
               + (dir2 ? `\x1b[34m${name}/\x1b[0m` : name);
        }).join('\r\n'));
      } else {
        send(entries.map(name => {
          const isDir = fs.statSync(path.join(dir, name)).isDirectory();
          return isDir ? `\x1b[34m${name}/\x1b[0m` : name;
        }).join('  '));
      }
    } catch (e) { send(`\x1b[31mls: ${e.message}\x1b[0m`); }
  }

  function cmdCd(args) {
    const t = args[0] || '~';
    if (t === '~' || t === '/') { cwd = WORKSPACE; return; }
    const p = safe(t);
    if (!p)                      { send(`\x1b[31mcd: permission denied\x1b[0m`); return; }
    if (!fs.existsSync(p))       { send(`\x1b[31mcd: ${t}: No such file or directory\x1b[0m`); return; }
    if (!fs.statSync(p).isDirectory()) { send(`\x1b[31mcd: ${t}: Not a directory\x1b[0m`); return; }
    cwd = p;
  }

  function cmdPwd() {
    send('/' + path.relative(WORKSPACE, cwd).replace(/\\/g, '/'));
  }

  function cmdCat(args) {
    if (!args[0]) { send('\x1b[31mcat: missing operand\x1b[0m'); return; }
    const p = safe(args[0]);
    if (!p)                { send('\x1b[31mcat: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(p)) { send(`\x1b[31mcat: ${args[0]}: No such file or directory\x1b[0m`); return; }
    try { send(fs.readFileSync(p, 'utf8').replace(/\n/g, '\r\n')); }
    catch (e) { send(`\x1b[31mcat: ${e.message}\x1b[0m`); }
  }

  function cmdMkdir(args) {
    const targets = args.filter(a => !a.startsWith('-'));
    if (!targets.length) { send('\x1b[31mmkdir: missing operand\x1b[0m'); return; }
    for (const t of targets) {
      const p = safe(t);
      if (!p) { send('\x1b[31mmkdir: permission denied\x1b[0m'); continue; }
      try { fs.mkdirSync(p, { recursive: true }); notifyFsChange('mkdir', [rel(p)]); }
      catch (e) { send(`\x1b[31mmkdir: ${e.message}\x1b[0m`); }
    }
  }

  function cmdRm(args) {
    const recursive = args.some(a => /^-\w*r/.test(a));
    const targets   = args.filter(a => !a.startsWith('-'));
    if (!targets.length) { send('\x1b[31mrm: missing operand\x1b[0m'); return; }
    for (const t of targets) {
      const p = safe(t);
      if (!p)                { send(`\x1b[31mrm: '${t}': Permission denied\x1b[0m`); continue; }
      if (!fs.existsSync(p)) { send(`\x1b[31mrm: '${t}': No such file or directory\x1b[0m`); continue; }
      try { fs.rmSync(p, { recursive, force: true }); notifyFsChange('delete', [rel(p)]); }
      catch (e) { send(`\x1b[31mrm: ${e.message}\x1b[0m`); }
    }
  }

  function cmdCp(args) {
    const targets = args.filter(a => !a.startsWith('-'));
    if (targets.length < 2) { send('\x1b[31mcp: missing destination\x1b[0m'); return; }
    const src = safe(targets[0]), dst = safe(targets[1]);
    if (!src || !dst)       { send('\x1b[31mcp: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(src)){ send(`\x1b[31mcp: '${targets[0]}': No such file or directory\x1b[0m`); return; }
    try { fs.copyFileSync(src, dst); notifyFsChange('update', [rel(dst)]); }
    catch (e) { send(`\x1b[31mcp: ${e.message}\x1b[0m`); }
  }

  function cmdMv(args) {
    const targets = args.filter(a => !a.startsWith('-'));
    if (targets.length < 2) { send('\x1b[31mmv: missing destination\x1b[0m'); return; }
    const src = safe(targets[0]), dst = safe(targets[1]);
    if (!src || !dst)       { send('\x1b[31mmv: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(src)){ send(`\x1b[31mmv: '${targets[0]}': No such file or directory\x1b[0m`); return; }
    try {
      const srcRel = rel(src);
      fs.renameSync(src, dst);
      notifyFsChange('delete', [srcRel]);
      notifyFsChange('update', [rel(dst)]);
    }
    catch (e) { send(`\x1b[31mmv: ${e.message}\x1b[0m`); }
  }

  function cmdTouch(args) {
    if (!args[0]) { send('\x1b[31mtouch: missing operand\x1b[0m'); return; }
    for (const t of args) {
      const p = safe(t);
      if (!p) { send('\x1b[31mtouch: permission denied\x1b[0m'); continue; }
      try {
        if (fs.existsSync(p)) { const n = new Date(); fs.utimesSync(p, n, n); }
        else                  { fs.writeFileSync(p, ''); }
        notifyFsChange('update', [rel(p)]);
      } catch (e) { send(`\x1b[31mtouch: ${e.message}\x1b[0m`); }
    }
  }

  function cmdEcho(args) {
    const ri = args.indexOf('>'), ai = args.indexOf('>>');
    if (ri !== -1) {
      const file = args[ri + 1];
      if (!file) { send('\x1b[31mecho: missing filename after >\x1b[0m'); return; }
      const p = safe(file);
      if (!p) { send('\x1b[31mecho: permission denied\x1b[0m'); return; }
      try { fs.writeFileSync(p, args.slice(0, ri).join(' ') + '\n'); notifyFsChange('update', [rel(p)]); }
      catch (e) { send(`\x1b[31mecho: ${e.message}\x1b[0m`); }
    } else if (ai !== -1) {
      const file = args[ai + 1];
      if (!file) { send('\x1b[31mecho: missing filename after >>\x1b[0m'); return; }
      const p = safe(file);
      if (!p) { send('\x1b[31mecho: permission denied\x1b[0m'); return; }
      try { fs.appendFileSync(p, args.slice(0, ai).join(' ') + '\n'); notifyFsChange('update', [rel(p)]); }
      catch (e) { send(`\x1b[31mecho: ${e.message}\x1b[0m`); }
    } else {
      send(args.join(' '));
    }
  }

  async function cmdNovacomp(args) {
    // Parse: novacomp <file.nova> [-o <outname>] [--optimize] [--dump-ir]
    let fileArg  = null;
    let outName  = 'exe';
    const extraArgs = [];
    for (let i = 0; i < args.length; i++) {
      if (args[i] === '-o' && args[i + 1]) { outName = args[++i]; }
      else if (args[i].startsWith('--'))    { extraArgs.push(args[i]); }
      else if (!fileArg)                    { fileArg = args[i]; }
    }

    if (!fileArg) {
      send('\x1b[31musage: novacomp <file.nova> [-o <name>] [--optimize] [--dump-ir]\x1b[0m');
      return;
    }

    const p = safe(fileArg);
    if (!p)                { send('\x1b[31mnovacomp: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(p)) { send(`\x1b[31mnovacomp: '${fileArg}': No such file\x1b[0m`); return; }

    // Write the compiled artifact — a JSON metadata file the runtime uses.
    // When x86 codegen lands (v9), this becomes a real binary in place.
    // When JIT lands (v10), this becomes a bytecode (.nbc) file.
    const exePath = path.join(cwd, outName);
    const meta = {
      source:    path.relative(userWorkspace, p).replace(/\\/g, '/'),
      extraArgs,
      novacomp:  '1.0',
      compiled:  new Date().toISOString(),
    };
    try {
      fs.writeFileSync(exePath, JSON.stringify(meta, null, 2), 'utf8');
    } catch (e) {
      send(`\x1b[31mnovacomp: could not write '${outName}': ${e.message}\x1b[0m`);
      return;
    }

    send(`\x1b[32mCompiled\x1b[0m  ${fileArg}  →  \x1b[33m${outName}\x1b[0m\r\n\x1b[2mRun with:  ./${outName} [input1 input2 ...]\x1b[0m`);
  }

  // Run a compiled novacomp artifact: ./exe [input1 input2 ...]
  // Each positional arg is fed as one line on stdin (for input() calls).
  async function cmdRunExe(name, inputs) {
    const exeName = name.replace(/^\.\//, '');
    const exePath = safe(exeName);
    if (!exePath || !fs.existsSync(exePath)) {
      send(`\x1b[31mbash: ./${exeName}: No such file\x1b[0m`); return;
    }

    let meta;
    try {
      meta = JSON.parse(fs.readFileSync(exePath, 'utf8'));
      if (!meta.source || !meta.novacomp) throw new Error('not a novacomp artifact');
    } catch {
      send(`\x1b[31m./${exeName}: not a novacomp executable — run 'novacomp <file.nova>' first\x1b[0m`);
      return;
    }

    const srcPath = safe(meta.source);
    if (!srcPath || !fs.existsSync(srcPath)) {
      send(`\x1b[31mSource '${meta.source}' not found. Recompile with novacomp.\x1b[0m`); return;
    }

    // The compiler binary requires a .csl extension — write source to a temp file.
    const { tmpDir, tmpFile } = writeTempFile(fs.readFileSync(srcPath, 'utf8'));

    return new Promise(resolve => {
      const proc = spawn(BINARY, [tmpFile, ...(meta.extraArgs || [])], {
        stdio: ['pipe', 'pipe', 'pipe'],
        shell: process.platform === 'win32'
      });
      currentProc = proc;
      proc.__killed = false;

      // Feed each input arg as one stdin line (for input() calls)
      if (inputs.length > 0) {
        proc.stdin.write(inputs.join('\n') + '\n', () => proc.stdin.end());
      } else {
        proc.stdin.end();
      }

      proc.stdout.on('data', d => send(d.toString().replace(/\n/g, '\r\n')));
      proc.stderr.on('data', d => {
        const f = filterStderr(d.toString());
        if (f) send('\x1b[31m' + f.replace(/\n/g, '\r\n') + '\x1b[0m');
      });
      proc.on('close', code => {
        currentProc = null;
        fs.rmSync(tmpDir, { recursive: true, force: true });
        if (!proc.__killed && code !== 0 && code !== null)
          send(`\x1b[2m[process exited with code ${code}]\x1b[0m`);
        resolve();
      });
    });
  }

  function cmdHelp() {
    send([
      '\x1b[33m── NovaComp Workspace Terminal ─────────────────────────\x1b[0m',
      '  Sandboxed to the IDE workspace — cannot access your machine.',
      '',
      '  \x1b[36mls\x1b[0m [-l] [-a]            List directory',
      '  \x1b[36mcd\x1b[0m <dir>                Change directory  (~ = root)',
      '  \x1b[36mpwd\x1b[0m                      Print working directory',
      '  \x1b[36mcat\x1b[0m <file>              Show file contents',
      '  \x1b[36mtouch\x1b[0m <file>            Create empty file',
      '  \x1b[36mmkdir\x1b[0m <dir>             Create directory',
      '  \x1b[36mrm\x1b[0m [-r] <path>           Remove file or directory',
      '  \x1b[36mcp\x1b[0m <src> <dst>          Copy file',
      '  \x1b[36mmv\x1b[0m <src> <dst>          Move / rename',
      '  \x1b[36mecho\x1b[0m <text>             Print text',
      '  \x1b[36mecho\x1b[0m <text> > <file>    Write to file',
      '  \x1b[36mnovacomp\x1b[0m <file.nova>              Compile → produces \x1b[33mexe\x1b[0m',
      '  \x1b[36mnovacomp\x1b[0m <file.nova> -o <name>    Compile → produces \x1b[33m<name>\x1b[0m',
      '  \x1b[36m./<name>\x1b[0m [input1 input2 ...]       Run compiled program',
      '  \x1b[36mclear\x1b[0m                             Clear screen',
      '  \x1b[36mhelp\x1b[0m                              Show this help',
      '\x1b[33m────────────────────────────────────────────────────────\x1b[0m',
    ].join('\r\n'));
  }

  // ── command dispatcher ────────────────────────────────────────────────────

  async function execute(line) {
    const trimmed = line.trim();
    if (!trimmed) return;
    if (!history.length || history[history.length - 1] !== trimmed)
      history.push(trimmed);
    histIdx = -1; histDraft = '';

    // Tokenise (handles "quoted args")
    const parts = trimmed.match(/(?:[^\s"']+|"[^"]*"|'[^']*')+/g) || [];
    const cmd   = parts[0];
    const args  = parts.slice(1).map(a => a.replace(/^["']|["']$/g, ''));

    switch (cmd) {
      case 'ls': case 'll': cmdLs(cmd === 'll' ? ['-l', ...args] : args); break;
      case 'cd':     cmdCd(args);   break;
      case 'pwd':    cmdPwd();      break;
      case 'cat':    cmdCat(args);  break;
      case 'touch':  cmdTouch(args);break;
      case 'mkdir':  cmdMkdir(args);break;
      case 'rm':     cmdRm(args);   break;
      case 'cp':     cmdCp(args);   break;
      case 'mv':     cmdMv(args);   break;
      case 'echo':   cmdEcho(args); break;
      case 'clear':  send('\x1b[2J\x1b[H'); break;
      case 'help':   cmdHelp();     break;
      case 'novacomp': await cmdNovacomp(args); break;
      default:
        if (cmd && cmd.startsWith('./')) {
          await cmdRunExe(cmd, args);
        } else {
          send(`\x1b[31m${cmd}: command not found\x1b[0m\r\nType \x1b[36mhelp\x1b[0m to see available commands.`);
        }
    }
  }

  // ── keystroke processor ───────────────────────────────────────────────────

  async function processInput(data) {
    let i = 0;
    while (i < data.length) {
      const char = data[i];
      const code = data.charCodeAt(i);

      if (char === '\r' || char === '\n') {
        send('\r\n');
        await execute(inputBuf);
        inputBuf = '';
        showPrompt();
      } else if (char === '\x7f' || char === '\b') {          // Backspace
        if (inputBuf.length > 0) { inputBuf = inputBuf.slice(0, -1); send('\b \b'); }
      } else if (char === '\x03') {                            // Ctrl+C
        if (currentProc) {
          currentProc.__killed = true;
          try { currentProc.kill(); } catch (_) {}
          send('\r\n\x1b[2m[killed]\x1b[0m');
          // showPrompt() will be called by the awaiting execute() once the proc closes
        } else {
          send('^C'); inputBuf = ''; showPrompt();
        }
      } else if (char === '\x0c') {                            // Ctrl+L
        send('\x1b[2J\x1b[H'); showPrompt(); if (inputBuf) send(inputBuf);
      } else if (char === '\x1b' && i + 2 < data.length && data[i + 1] === '[') {
        const arrow = data[i + 2];
        if (arrow === 'A' && history.length) {                 // Up
          if (histIdx === -1) { histDraft = inputBuf; histIdx = history.length - 1; }
          else if (histIdx > 0) histIdx--;
          replaceInput(history[histIdx]);
        } else if (arrow === 'B') {                            // Down
          if (histIdx !== -1) {
            histIdx < history.length - 1 ? histIdx++ : (histIdx = -1);
            replaceInput(histIdx === -1 ? histDraft : history[histIdx]);
          }
        }
        i += 2;
      } else if (code >= 0x20 && code !== 0x7f) {             // Printable
        inputBuf += char; send(char);
      }
      i++;
    }
  }

  // ── sync user's S3 files into their workspace, then show prompt ──────────

  if (userId) {
    send('\x1b[2mLoading your workspace…\x1b[0m');
    try {
      const { Contents = [] } = await s3.send(new ListObjectsV2Command({
        Bucket: S3_BUCKET, Prefix: `${userId}/`,
      }));
      for (const obj of Contents) {
        const fileName = path.basename(obj.Key.replace(`${userId}/`, ''));
        if (!fileName) continue;
        const localPath = path.join(userWorkspace, fileName);
        try {
          if (!fs.existsSync(localPath) ||
              new Date(obj.LastModified) > fs.statSync(localPath).mtime) {
            const { Body } = await s3.send(new GetObjectCommand({ Bucket: S3_BUCKET, Key: obj.Key }));
            fs.writeFileSync(localPath, await Body.transformToString(), 'utf8');
          }
        } catch (_) {}
      }
    } catch (e) {
      log('error', 'Terminal S3 sync failed', { error: e.message });
    }
    send('\r\x1b[K'); // erase the "Loading" line
  }

  send('\x1b[32mNovaComp IDE\x1b[0m\r\n');
  showPrompt();

  ws.on('message', raw => {
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }
    if (msg.type === 'input') {
      processInput(msg.data).catch(() => {});
    } else if (msg.type === 'write' && msg.fileName) {
      const p = safe(path.basename(String(msg.fileName)));
      if (p) try { fs.writeFileSync(p, String(msg.content ?? ''), 'utf8'); } catch (_) {}
    }
  });

  ws.on('close', () => log('info', 'Shell session closed'));
  ws.on('error', err => log('error', 'Shell WebSocket error', { error: err.message }));
}

// ── Terminal connection handler ───────────────────────────────────────────────

termWss.on('connection', (ws, req) => {
  const userId = req._userId || null;
  log('info', 'Terminal connection', { ip: req.socket.remoteAddress, userId });
  createShellSession(ws, userId);
});

// ── Share endpoints ───────────────────────────────────────────────────────────

// POST /api/share  { code }
app.post('/api/share', async (req, res) => {
  const { code } = req.body || {};
  if (!code || typeof code !== 'string')
    return res.status(400).json({ error: 'code required' });
  if (code.length > MAX_SOURCE_LENGTH)
    return res.status(400).json({ error: `Code too long (max ${MAX_SOURCE_LENGTH} chars)` });

  const id  = crypto.randomUUID();
  const key = `shares/${id}`;
  try {
    await s3.send(new PutObjectCommand({
      Bucket: S3_BUCKET, Key: key, Body: code, ContentType: 'text/plain',
    }));
    res.json({ id });
  } catch (e) {
    log('error', 'Share save failed', { error: e.message });
    res.status(500).json({ error: 'Failed to create share' });
  }
});

// GET /api/share/:id  (no auth required)
app.get('/api/share/:id', async (req, res) => {
  const { id } = req.params;
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(id))
    return res.status(400).json({ error: 'invalid id' });
  try {
    const { Body } = await s3.send(new GetObjectCommand({ Bucket: S3_BUCKET, Key: `shares/${id}` }));
    const code = await Body.transformToString();
    res.json({ code, id });
  } catch (e) {
    if (e.name === 'NoSuchKey') return res.status(404).json({ error: 'Share not found' });
    res.status(500).json({ error: 'Failed to load share' });
  }
});

// ── SPA fallback — serve React app for all non-API routes ────────────────────
app.use((req, res) => {
  res.sendFile(path.join(__dirname, 'dist', 'index.html'));
});

// ── Start (use server.listen instead of app.listen for WebSocket support) ────

server.listen(PORT, () => {
  ensureTables().catch(e => log("error", "DynamoDB table init failed", { error: e.message }));

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