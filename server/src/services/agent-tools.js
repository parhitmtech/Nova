'use strict';
// Tools NovaBot can call (run code, read/write/list the user's files).
const { S3_BUCKET, FILES_TABLE, MAX_EXECUTION_MS, ML_EXECUTION_MS } = require('../config');
const { dynamo, s3 } = require('./aws');
const { filterStderr, validateSource, writeTempFile, cleanupTempFile, runCompiler } = require('./compiler-runner');
const { PutCommand } = require('@aws-sdk/lib-dynamodb');
const { PutObjectCommand, GetObjectCommand, ListObjectsV2Command } = require('@aws-sdk/client-s3');

// ── executeTool: handle NovaBot tool calls ─────────────────────────────────

async function executeTool(name, input, userId) {
  if (name === 'run_nova_code') {
    const { code, inputs = '' } = input;
    const invalid = validateSource(code);
    if (invalid) return { error: invalid };
    let tmpFile, tmpDir;
    try {
      ({ tmpFile, tmpDir } = writeTempFile(code));
      const timeout = /\/\/\s*@ml\b/.test(code) ? ML_EXECUTION_MS : MAX_EXECUTION_MS;
      const { stdout, stderr, ms, exitCode } = await runCompiler(tmpFile, [], inputs, timeout);
      const errOut = filterStderr(stderr);
      if (exitCode !== 0) return { error: errOut || 'Compilation failed', ms };
      return { output: stdout || '(no output)', ms };
    } catch (e) {
      return { error: e.message };
    } finally {
      cleanupTempFile(tmpFile, tmpDir);
    }
  }

  if (name === 'read_user_file') {
    const { fileName } = input;
    if (!fileName) return { error: 'fileName is required' };
    try {
      const obj = await s3.send(new GetObjectCommand({ Bucket: S3_BUCKET, Key: `${userId}/${fileName}` }));
      const chunks = [];
      for await (const chunk of obj.Body) chunks.push(chunk);
      return { content: Buffer.concat(chunks).toString('utf8') };
    } catch (e) {
      if (e.name === 'NoSuchKey') return { error: `File not found: ${fileName}` };
      return { error: e.message };
    }
  }

  if (name === 'write_user_file') {
    const { fileName, content } = input;
    if (!fileName || !content) return { error: 'fileName and content are required' };
    if (!fileName.endsWith('.nova')) return { error: 'File must end in .nova' };
    const key = `${userId}/${fileName}`;
    try {
      await s3.send(new PutObjectCommand({ Bucket: S3_BUCKET, Key: key, Body: content, ContentType: 'text/plain' }));
      await dynamo.send(new PutCommand({
        TableName: FILES_TABLE,
        Item: { userId, filePath: fileName, updatedAt: Date.now() },
      }));
      return { success: true };
    } catch (e) {
      return { error: e.message };
    }
  }

  if (name === 'list_user_files') {
    try {
      const result = await s3.send(new ListObjectsV2Command({ Bucket: S3_BUCKET, Prefix: `${userId}/` }));
      const files = (result.Contents || []).map(obj => obj.Key.replace(`${userId}/`, ''));
      return { files };
    } catch (e) {
      return { error: e.message };
    }
  }

  if (name === 'explain_error') {
    const { error, code } = input;
    return { acknowledged: true, error, code };
  }

  return { error: `Unknown tool: ${name}` };
}

module.exports = { executeTool };
