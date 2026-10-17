'use strict';
// GET /health.
const express = require('express');
const fs = require('fs');
const { BINARY, MAX_CONCURRENT } = require('../config');
const { executions } = require('../services/compiler-runner');
const { replSessions } = require('../ws/repl');
const { pty, resolveShell } = require('../ws/terminal');
const { MODEL_ID } = require('../agent/bedrock-client');

const router = express.Router();

// ── GET /health ───────────────────────────────────────────────────────────────

router.get("/health", (_req, res) => {
  const binaryExists = fs.existsSync(BINARY);
  res.json({
    status:          binaryExists ? "ok" : "binary_missing",
    binary:          BINARY,
    exists:          binaryExists,
    activeExecutions: executions.active,
    maxConcurrent:   MAX_CONCURRENT,
    uptime:          process.uptime(),
    replSessions:    replSessions.size,
    ptyLoaded:       !!pty,
    terminalShell:   resolveShell().shell,
    bedrockModel:    process.env.BEDROCK_MODEL_ID || MODEL_ID,
    bedrockRegion:   process.env.BEDROCK_REGION   || 'us-east-1',
  });
});

module.exports = router;
