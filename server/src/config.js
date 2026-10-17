'use strict';
// Server settings read from the environment, plus fixed execution limits.
const fs = require('fs');
const security = require('./services/security');
const paths = require('./paths');

const PORT = parseInt(process.env.PORT || "3001", 10);

// ── AWS ───────────────────────────────────────────────────────────────────────

const AWS_REGION  = process.env.AWS_REGION        || "us-east-1";
const S3_BUCKET   = process.env.S3_BUCKET         || "novacomp-files";
const USERS_TABLE         = process.env.DYNAMODB_TABLE_USERS          || "novacomp-users";
const FILES_TABLE         = "novacomp-files-meta";
const AGENT_HISTORY_TABLE = process.env.DYNAMODB_TABLE_AGENT_HISTORY  || "novacomp-agent-history";
const AGENT_MEMORY_TABLE  = process.env.DYNAMODB_TABLE_AGENT_MEMORY   || "novacomp-agent-memory";
const JWT_SECRET          = security.validateSecret(process.env.JWT_SECRET);

const BINARY = paths.COMPILER_PATH;

// ── Workspace (persistent user files) ────────────────────────────────────────
const WORKSPACE     = paths.WORKSPACE_DIR;
if (!fs.existsSync(WORKSPACE)) fs.mkdirSync(WORKSPACE, { recursive: true });

// ── Limits ────────────────────────────────────────────────────────────────────

const MAX_SOURCE_LENGTH  = 10_000;
const MAX_EXECUTION_MS   = Math.min(30_000, Math.max(1, Number(process.env.NOVA_EXECUTION_TIMEOUT_MS) || 30_000));
const ML_EXECUTION_MS    = MAX_EXECUTION_MS; // annotations cannot raise web execution limits
const MAX_OUTPUT_BYTES   = 256_000;
const MAX_CONCURRENT     = 5;
const BENCH_ITERS        = 10000;
const REPL_IDLE_TIMEOUT  = 5 * 60 * 1000;  // 5 minutes

// All web execution shares the same hard deadline, including ML annotations.
function getExecutionTimeout(_source) { return MAX_EXECUTION_MS; }

module.exports = {
  PORT, AWS_REGION, S3_BUCKET, USERS_TABLE, FILES_TABLE, AGENT_HISTORY_TABLE, AGENT_MEMORY_TABLE,
  JWT_SECRET, BINARY, WORKSPACE, MAX_SOURCE_LENGTH, MAX_EXECUTION_MS, ML_EXECUTION_MS,
  MAX_OUTPUT_BYTES, MAX_CONCURRENT, BENCH_ITERS, REPL_IDLE_TIMEOUT, getExecutionTimeout,
};
