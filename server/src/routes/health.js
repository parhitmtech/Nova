'use strict';
// GET /health.
const express = require('express');

const router = express.Router();

// ── GET /health ───────────────────────────────────────────────────────────────
// A public liveness probe: it reveals nothing about binaries, sessions or providers.

router.get("/health", (_req, res) => res.json({ status: 'ok' }));

module.exports = router;
