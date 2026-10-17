'use strict';
// WebSocket REPL: one isolated `nova --repl` process per connection.
const WebSocket = require('ws');
const { MAX_SOURCE_LENGTH, MAX_OUTPUT_BYTES, REPL_IDLE_TIMEOUT } = require('../config');
const { log } = require('../lib/logger');
const { writeTempFile, cleanupTempFile } = require('../services/compiler-runner');
const { spawnCompiler } = require('../services/execution');

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
  const { tmpDir } = writeTempFile('');
  log("info", "REPL session started", { activeSessions: replSessions.size + 1 });

  // spawn the compiler in REPL mode
  let child;
  try { child = spawnCompiler(["--repl"], { cwd: tmpDir, timeoutMs: REPL_IDLE_TIMEOUT }); }
  catch (err) { cleanupTempFile(null, tmpDir); ws.close(1013, 'Execution unavailable'); return; }

  // ── helper: send JSON message to browser ──────────────────────────────────
  function send(type, text) {
    if (ws.readyState === WebSocket.OPEN) {
      if (ws.bufferedAmount > MAX_OUTPUT_BYTES) ws.close(1009, 'Output limit exceeded');
      else ws.send(JSON.stringify({ type, text }));
    }
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
    const session = replSessions.get(ws);
    if (session?._idleTimer) clearTimeout(session._idleTimer);
    replSessions.delete(ws);
    cleanupTempFile(null, tmpDir);
  });

  child.on("error", (err) => {
    log("error", "REPL process error", { error: err.message });
    send("error", `\n  Process error: ${err.message}\n`);
    send("closed", "");
    const session = replSessions.get(ws);
    if (session?._idleTimer) clearTimeout(session._idleTimer);
    replSessions.delete(ws);
    cleanupTempFile(null, tmpDir);
  });

  // ── store session ─────────────────────────────────────────────────────────
  replSessions.set(ws, { child, idleTimer: () => idleTimer });

  // ── notify browser that session is ready ─────────────────────────────────
  send("ready", "");
}

// ── REPL connection handler ───────────────────────────────────────────────────

function handleReplConnection(ws, req) {
  log("info", "WebSocket REPL connection opened", {
    ip: req.socket.remoteAddress,
    activeSessions: replSessions.size + 1
  });

  createReplSession(ws);
  let inputBytes = 0;

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

      if (typeof msg.line !== "string" || msg.line.length > MAX_SOURCE_LENGTH) return ws.close(1009, "Input limit exceeded");
      const line = msg.line + "\n";
      inputBytes += Buffer.byteLength(line);
      if (inputBytes > 64000) return ws.close(1009, "Session input limit exceeded");
      try { if (!session.child.stdin.write(line)) ws.close(1009, "Input buffer full"); } catch (_) { ws.close(); }
    }
  });

  ws.on("close", () => {
    log("info", "WebSocket REPL connection closed");
    const session = replSessions.get(ws);
    if (session) {
      if (session._idleTimer) clearTimeout(session._idleTimer);
      if (session.idleTimer) clearTimeout(session.idleTimer());
      try { session.child.kill(); } catch (_) {}
      replSessions.delete(ws);
    }
  });

  ws.on("error", (err) => {
    log("error", "WebSocket REPL error", { error: err.message });
  });
}

module.exports = { replSessions, handleReplConnection };
