'use strict';
// Code sharing: POST /api/share, GET /api/share/:id.
const express = require('express');
const crypto = require('crypto');
const { S3_BUCKET, MAX_SOURCE_LENGTH } = require('../config');
const { log } = require('../lib/logger');
const { s3 } = require('../services/aws');
const { PutObjectCommand, GetObjectCommand } = require('@aws-sdk/client-s3');

const router = express.Router();

// ── Share endpoints ───────────────────────────────────────────────────────────

// POST /api/share  { code }
router.post('/api/share', async (req, res) => {
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
router.get('/api/share/:id', async (req, res) => {
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

module.exports = router;
