'use strict';
// POST /auth/signup, POST /auth/login, GET /auth/me.
const express = require('express');
const crypto = require('crypto');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');
const { USERS_TABLE, JWT_SECRET } = require('../config');
const { log } = require('../lib/logger');
const { dynamo } = require('../services/aws');
const { requireAuth } = require('../middleware/auth');
const { PutCommand, GetCommand } = require('@aws-sdk/lib-dynamodb');

const router = express.Router();

// ── Auth Endpoints ────────────────────────────────────────────────────────────

// POST /auth/signup  { email, password }
router.post("/auth/signup", async (req, res) => {
  const { email, password } = req.body || {};
  if (typeof email !== 'string' || email.length > 254 || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email) || typeof password !== 'string' || password.length < 8 || Buffer.byteLength(password) > 72)
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

    const token = jwt.sign({ userId, email }, JWT_SECRET, { expiresIn: "7d", algorithm: "HS256", issuer: "nova", audience: "nova-ide" });
    res.json({ token, user: { userId, email, createdAt } });
  } catch (e) {
    log("error", "Signup failed", { error: e.message });
    res.status(500).json({ error: "Signup failed" });
  }
});

// POST /auth/login  { email, password }
router.post("/auth/login", async (req, res) => {
  const { email, password } = req.body || {};
  if (typeof email !== 'string' || email.length > 254 || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email) || typeof password !== 'string' || password.length === 0 || Buffer.byteLength(password) > 4096)
    return res.status(400).json({ error: "email and password required" });

  try {
    const result = await dynamo.send(new GetCommand({ TableName: USERS_TABLE, Key: { email } }));
    if (!result.Item) return res.status(401).json({ error: "Invalid credentials" });

    const valid = await bcrypt.compare(password, result.Item.passwordHash);
    if (!valid) return res.status(401).json({ error: "Invalid credentials" });

    const { userId, createdAt } = result.Item;
    const token = jwt.sign({ userId, email }, JWT_SECRET, { expiresIn: "7d", algorithm: "HS256", issuer: "nova", audience: "nova-ide" });
    res.json({ token, user: { userId, email, createdAt } });
  } catch (e) {
    log("error", "Login failed", { error: e.message });
    res.status(500).json({ error: "Login failed" });
  }
});

// GET /auth/me  (requires Bearer token)
router.get("/auth/me", requireAuth, (req, res) => {
  res.json({ user: req.user });
});

module.exports = router;
