'use strict';
// Server settings read from the environment, plus fixed execution limits.
const fs = require('fs');
const security = require('./services/security');
const paths = require('./paths');
const { log } = require('./lib/logger');

const PORT = parseInt(process.env.PORT || "3001", 10);

// ── Deployment checks (run before accepting traffic) ─────────────────────────

// Validate origin, proxy and worker settings. Throws on a configuration that is
// unsafe to serve; returns the parsed values plus any warnings to log.
function checkDeployment(env) {
  const production = env.NODE_ENV === 'production';
  const warnings = [];
  const allowedOrigins = (env.NOVA_ALLOWED_ORIGINS || '').split(',').map(s => s.trim()).filter(Boolean);
  if (production && !allowedOrigins.length)
    throw new Error('NOVA_ALLOWED_ORIGINS must list the public HTTPS origins in production');
  for (const origin of allowedOrigins) {
    let parsed;
    try { parsed = new URL(origin); } catch { throw new Error('Invalid NOVA_ALLOWED_ORIGINS'); }
    if (!['http:', 'https:'].includes(parsed.protocol) || parsed.origin !== origin) throw new Error('Invalid NOVA_ALLOWED_ORIGINS');
    // Plain http (localhost included) is for development only.
    if (production && parsed.protocol !== 'https:')
      throw new Error(`NOVA_ALLOWED_ORIGINS must contain only https: origins in production (got ${origin})`);
  }

  // Express 'trust proxy' value: false, a hop count, or explicit proxy addresses/CIDRs.
  const trustedProxy = env.NOVA_TRUST_PROXY || '';
  if (trustedProxy === 'true') throw new Error('NOVA_TRUST_PROXY must specify trusted proxy addresses or a hop count');
  const trustProxy = !trustedProxy || trustedProxy === 'false' ? false
    : /^\d+$/.test(trustedProxy) ? Number(trustedProxy) : trustedProxy.split(',').map(s => s.trim());
  if (production && !trustedProxy)
    warnings.push('NOVA_TRUST_PROXY is unset: behind a TLS proxy every client shares the proxy address for rate limits. Set it to the proxy address/CIDR or hop count.');

  // The start-up container sweep is scoped by this ID; a shared default would
  // let one instance's sweep target another's containers.
  if (production && !env.NOVA_WORKER_ID)
    throw new Error('NOVA_WORKER_ID must be set to a stable, unique backend identity in production');
  return { allowedOrigins, trustProxy, warnings };
}

const deployment = checkDeployment(process.env);
for (const message of deployment.warnings) log('warn', message);
const ALLOWED_ORIGINS = deployment.allowedOrigins;
const TRUST_PROXY = deployment.trustProxy;

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
let WORKSPACE     = paths.WORKSPACE_DIR;
if (!fs.existsSync(WORKSPACE)) fs.mkdirSync(WORKSPACE, { recursive: true });
WORKSPACE = fs.realpathSync(WORKSPACE); // a symlinked ancestor must not defeat path checks

// ── Limits ────────────────────────────────────────────────────────────────────

const MAX_SOURCE_LENGTH  = 10_000;
const MAX_EXECUTION_MS   = Math.min(30_000, Math.max(1, Number(process.env.NOVA_EXECUTION_TIMEOUT_MS) || 30_000));
const ML_EXECUTION_MS    = MAX_EXECUTION_MS; // annotations cannot raise web execution limits
const MAX_OUTPUT_BYTES   = 256_000;
const MAX_CONCURRENT     = 5;
const BENCH_ITERS        = 10000;
const REPL_IDLE_TIMEOUT  = Math.min(5 * 60 * 1000, Math.max(1, Number(process.env.NOVA_REPL_IDLE_TIMEOUT_MS) || 5 * 60 * 1000));  // 5 minutes

// All web execution shares the same hard deadline, including ML annotations.
function getExecutionTimeout(_source) { return MAX_EXECUTION_MS; }

module.exports = {
  PORT, ALLOWED_ORIGINS, TRUST_PROXY, checkDeployment, AWS_REGION, S3_BUCKET, USERS_TABLE, FILES_TABLE, AGENT_HISTORY_TABLE, AGENT_MEMORY_TABLE,
  JWT_SECRET, BINARY, WORKSPACE, MAX_SOURCE_LENGTH, MAX_EXECUTION_MS, ML_EXECUTION_MS,
  MAX_OUTPUT_BYTES, MAX_CONCURRENT, BENCH_ITERS, REPL_IDLE_TIMEOUT, getExecutionTimeout,
};
