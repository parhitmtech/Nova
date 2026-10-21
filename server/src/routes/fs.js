'use strict';
// Per-user workspace filesystem (/fs/*) and the disabled /git/status.
const express = require('express');
const fs = require('fs');
const path = require('path');
const security = require('../services/security');
const { WORKSPACE } = require('../config');
const { log } = require('../lib/logger');
const { createWorkspaceStore } = require('../services/workspace-store');

const router = express.Router();

// ── Filesystem helpers ────────────────────────────────────────────────────────

function safeWorkspacePath(req, rel) {
  return security.safePath(security.userWorkspace(WORKSPACE, req.user.userId), rel);
}
// Every persistent mutation goes through the store so storage quotas apply.
function workspaceStore(req) {
  return createWorkspaceStore(security.userWorkspace(WORKSPACE, req.user.userId));
}

// ── POST /fs/write  { path, content } ────────────────────────────────────────
router.post("/fs/write", (req, res) => {
  const target = safeWorkspacePath(req, req.body?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    workspaceStore(req).write(target, req.body?.content ?? "");
    res.json({ ok: true });
  } catch (e) {
    res.status(e.status || 500).json({ error: e.message });
  }
});

// ── DELETE /fs/delete?path=... ────────────────────────────────────────────────
router.delete("/fs/delete", (req, res) => {
  const target = safeWorkspacePath(req, req.query?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    fs.rmSync(target, { recursive: true, force: true });
    res.json({ ok: true });
  } catch (e) {
    res.status(e.status || 500).json({ error: e.message });
  }
});

// ── POST /fs/mkdir  { path } ──────────────────────────────────────────────────
router.post("/fs/mkdir", (req, res) => {
  const target = safeWorkspacePath(req, req.body?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    workspaceStore(req).mkdir(target);
    res.json({ ok: true });
  } catch (e) {
    res.status(e.status || 500).json({ error: e.message });
  }
});

// ── POST /fs/rename  { from, to } ────────────────────────────────────────────
router.post("/fs/rename", (req, res) => {
  const src  = safeWorkspacePath(req, req.body?.from);
  const dest = safeWorkspacePath(req, req.body?.to);
  if (!src || !dest) return res.status(400).json({ error: "Invalid path" });
  try {
    workspaceStore(req).rename(src, dest);
    res.json({ ok: true });
  } catch (e) {
    res.status(e.status || 500).json({ error: e.message });
  }
});

// ── GET /fs/read?path=... ─────────────────────────────────────────────────────
router.get("/fs/read", (req, res) => {
  const target = safeWorkspacePath(req, req.query?.path);
  if (!target) return res.status(400).json({ error: "Invalid path" });
  try {
    const content = fs.readFileSync(target, "utf8");
    res.json({ content });
  } catch (e) {
    res.status(404).json({ error: e.message });
  }
});

// ── GET /fs/tree ──────────────────────────────────────────────────────────────
router.get("/fs/tree", (req, res) => {
  const walk = (dir, prefix = '') => {
    const items = [];
    try {
      const entries = fs.readdirSync(dir, { withFileTypes: true });
      
      for (const entry of entries) {
        // Skip hidden files and node_modules
        if (entry.isSymbolicLink() || entry.name.startsWith('.') || entry.name === 'node_modules') continue;
        
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
    const files = walk(security.userWorkspace(WORKSPACE, req.user.userId));
    res.json({ files });
  } catch (err) {
    log("error", "Failed to walk workspace", { error: err.message });
    res.status(500).json({ error: err.message });
  }
});

// ── GET /git/status ───────────────────────────────────────────────────────────
router.get("/git/status", (_req, res) => res.status(501).json({ error: 'Git status requires an isolated user repository' }));

module.exports = router;
