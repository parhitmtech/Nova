/**
 * NovaComp backend entry point: HTTP server + WebSockets.
 *
 * Setup:
 *   1. Build binary:   make -C ../compiler        (from server/; produces compiler/build/nova)
 *   2. Install deps:   npm ci
 *   3. Start:          npm start
 *
 * Layout: app.js (middleware + routers), routes/, services/, ws/, config.js, paths.js.
 */

require("dotenv").config();

const { createExecutor } = require("./services/execution");
createExecutor(); // Reject an invalid or unsafe execution mode before accepting traffic.

const http = require("http");
const fs   = require("fs");
const app  = require("./app");
const { attachWebSockets } = require("./ws");
const { log } = require("./lib/logger");
const { ensureTables } = require("./services/aws");
const { writeTempFile, cleanupTempFile } = require("./services/compiler-runner");
const {
  PORT, BINARY, MAX_CONCURRENT, MAX_SOURCE_LENGTH, MAX_EXECUTION_MS, BENCH_ITERS, REPL_IDLE_TIMEOUT,
} = require("./config");

process.on("unhandledRejection", (reason) => {
  log("error", "Unhandled promise rejection", { reason: reason?.toString() });
});

process.on("uncaughtException", (err) => {
  log("error", "Uncaught exception", { error: err.message, stack: err.stack });
  setTimeout(() => process.exit(1), 5000);
});

// ── Create HTTP server (server.listen instead of app.listen for WebSocket support) ──

const server = http.createServer(app);
attachWebSockets(server);

if (require.main === module) server.listen(PORT, () => {
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
      fix:  "make -C compiler (from the repo root), or set NOVA_COMPILER_PATH",
    });
  }
});

module.exports = { app, server, cleanupTempFile, writeTempFile };
