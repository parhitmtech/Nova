'use strict';
// S3-backed user files: /files, /files/save, /files/load, /files/delete.
const express = require('express');
const path = require('path');
const { S3_BUCKET, FILES_TABLE } = require('../config');
const { log } = require('../lib/logger');
const { dynamo, s3 } = require('../services/aws');
const { requireAuth } = require('../middleware/auth');
const { PutCommand, DeleteCommand } = require('@aws-sdk/lib-dynamodb');
const { PutObjectCommand, GetObjectCommand, DeleteObjectCommand, ListObjectsV2Command } = require('@aws-sdk/client-s3');

const router = express.Router();

// ── S3 File Endpoints (auth-protected) ───────────────────────────────────────

// GET /files — list user's files (S3 userId/ prefix)
router.get("/files", requireAuth, async (req, res) => {
  try {
    const { Contents = [] } = await s3.send(new ListObjectsV2Command({
      Bucket: S3_BUCKET,
      Prefix: `${req.user.userId}/`,
    }));
    const files = Contents.map(obj => ({
      fileName:  obj.Key.replace(`${req.user.userId}/`, ""),
      updatedAt: obj.LastModified,
    }));
    res.json({ files });
  } catch (e) {
    log("error", "Failed to list files", { error: e.message });
    res.status(500).json({ error: "Failed to list files" });
  }
});

// POST /files/save  { fileName, content }
router.post("/files/save", requireAuth, async (req, res) => {
  const { fileName, content } = req.body || {};
  if (!fileName || content === undefined)
    return res.status(400).json({ error: "fileName and content required" });

  const safeName  = path.basename(fileName).replace(/[^a-zA-Z0-9._-]/g, "_");
  const key       = `${req.user.userId}/${safeName}`;
  const updatedAt = new Date().toISOString();

  try {
    await s3.send(new PutObjectCommand({
      Bucket:      S3_BUCKET,
      Key:         key,
      Body:        content,
      ContentType: "text/plain",
    }));
    await dynamo.send(new PutCommand({
      TableName: FILES_TABLE,
      Item: { userId: req.user.userId, filePath: safeName, fileName: safeName, updatedAt },
    }));
    res.json({ ok: true, fileName: safeName, updatedAt });
  } catch (e) {
    log("error", "Failed to save file", { error: e.message });
    res.status(500).json({ error: "Failed to save file" });
  }
});

// GET /files/load?fileName=x
router.get("/files/load", requireAuth, async (req, res) => {
  const fileName = req.query.fileName;
  if (!fileName) return res.status(400).json({ error: "fileName required" });

  const safeName = path.basename(fileName).replace(/[^a-zA-Z0-9._-]/g, "_");
  const key      = `${req.user.userId}/${safeName}`;

  try {
    const { Body } = await s3.send(new GetObjectCommand({ Bucket: S3_BUCKET, Key: key }));
    const content  = await Body.transformToString();
    res.json({ content, fileName: safeName });
  } catch (e) {
    if (e.name === "NoSuchKey") return res.status(404).json({ error: "File not found" });
    log("error", "Failed to load file", { error: e.message });
    res.status(500).json({ error: "Failed to load file" });
  }
});

// DELETE /files/delete?fileName=x
router.delete("/files/delete", requireAuth, async (req, res) => {
  const fileName = req.query.fileName;
  if (!fileName) return res.status(400).json({ error: "fileName required" });

  const safeName = path.basename(fileName).replace(/[^a-zA-Z0-9._-]/g, "_");
  const key      = `${req.user.userId}/${safeName}`;

  try {
    await s3.send(new DeleteObjectCommand({ Bucket: S3_BUCKET, Key: key }));
    await dynamo.send(new DeleteCommand({
      TableName: FILES_TABLE,
      Key: { userId: req.user.userId, filePath: safeName },
    }));
    res.json({ ok: true });
  } catch (e) {
    log("error", "Failed to delete file", { error: e.message });
    res.status(500).json({ error: "Failed to delete file" });
  }
});

module.exports = router;
