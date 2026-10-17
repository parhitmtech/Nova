'use strict';
// Running the Nova compiler for HTTP requests: input checks, per-job temp dirs, stderr filtering,
// the shared concurrency counter and benchmark parsing.
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const paths = require('../paths');
const { MAX_SOURCE_LENGTH, MAX_EXECUTION_MS } = require('../config');
const { spawnCompiler } = require('./execution');

// Number of compiler runs in flight across all routes (checked against MAX_CONCURRENT).
const executions = { active: 0 };

// ── Stderr Filter ─────────────────────────────────────────────────────────────
// Strip internal debug lines that should never reach the user.
const DEBUG_PREFIXES = [
  "PASSIGN:", "DEBUG OUT:", "DEBUG:", "EXEC START:", "EXEC OUT:",
  "EXEC ASSIGN_F:", "IR:", "FOLDS:", "REMOVED:", "BENCH_US:",
  "BENCH_NODES:", "BENCH_ITERS:",
  "NOVA_HOME=",
  "ASSIGN_D",
  "HF_CMD:", "HF_RESP:",
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

// Per-job temp dirs live under server/_tmp by default (NOVA_TEMP_DIR overrides).
const LOCAL_TEMP = paths.TEMP_DIR;
if (!fs.existsSync(LOCAL_TEMP)) fs.mkdirSync(LOCAL_TEMP, { recursive: true });

const activeTempDirs = new Set();
function writeTempFile(source) {
  const id = crypto.randomUUID();
  const tmpDir  = path.join(LOCAL_TEMP, id);
  const tmpFile = path.join(tmpDir, 'program.csl');
  fs.mkdirSync(tmpDir, { recursive: true, mode: 0o700 });
  activeTempDirs.add(tmpDir);
  fs.writeFileSync(tmpFile, source + '\n', 'utf8');
  return { tmpDir, tmpFile };
}

function cleanupTempFile(tmpFile, tmpDir) {
  if (!tmpDir || path.dirname(tmpDir) !== LOCAL_TEMP) return;
  activeTempDirs.delete(tmpDir);
  try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (_) {}
}
const cleanupTimer = setInterval(() => {
  for (const entry of fs.readdirSync(LOCAL_TEMP, { withFileTypes: true })) {
    const target = path.join(LOCAL_TEMP, entry.name);
    if (activeTempDirs.has(target) || entry.isSymbolicLink()) continue;
    try {
      if (entry.name.startsWith('gpu_job_') && entry.isFile()) {
        const meta = JSON.parse(fs.readFileSync(target, 'utf8'));
        if (['pending', 'running'].includes(meta.status)) continue;
      }
      if (Date.now() - fs.lstatSync(target).mtimeMs > 24 * 60 * 60 * 1000)
        fs.rmSync(target, { recursive: true, force: true });
    } catch (_) {}
  }
}, 60 * 60 * 1000);
cleanupTimer.unref();
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
    let timedOut = false;

    const child = spawnCompiler( [tmpFile, ...extraArgs], {
      stdio: ["pipe", "pipe", "pipe"],
      cwd: path.dirname(tmpFile),
      timeoutMs,
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
      timedOut = true;
      child.kill();
    }, timeoutMs);

    child.on("close", (code) => {
      clearTimeout(timer);
      const ms = Date.now() - start;
      console.log(`✅ Process closed: code=${code}, time=${ms}ms`);
      if (timedOut || child.stopReason === "timeout") reject({ type: "timeout", ms });
      else resolve({ stdout, stderr, ms, exitCode: code });
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

module.exports = {
  executions, filterStderr, sanitizeSource, validateSource, LOCAL_TEMP,
  writeTempFile, cleanupTempFile, runCompiler, parseBenchStats,
};
