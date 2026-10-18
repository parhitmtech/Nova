'use strict';
// Program execution: /run, /compile-bytecode, /run-stream (SSE), /compare.
const express = require('express');
const fs = require('fs');
const path = require('path');
const security = require('../services/security');
const { S3_BUCKET, FILES_TABLE, MAX_OUTPUT_BYTES, MAX_CONCURRENT, BENCH_ITERS, getExecutionTimeout } = require('../config');
const { log } = require('../lib/logger');
const { dynamo, s3 } = require('../services/aws');
const { executions, filterStderr, sanitizeSource, validateSource, writeTempFile, cleanupTempFile, runCompiler, parseBenchStats } = require('../services/compiler-runner');
const { spawnCompiler } = require('../services/execution');
const { PutCommand } = require('@aws-sdk/lib-dynamodb');
const { PutObjectCommand } = require('@aws-sdk/client-s3');

const router = express.Router();

// POST /run 

router.post("/run", async (req, res) => {
  const validationError = validateSource(req.body?.source);
  if (validationError) {
    log("warn", "Invalid request", { reason: validationError, ip: req.ip });
    return res.status(400).json({ error: validationError });
  }

  if (executions.active >= MAX_CONCURRENT) {
    log("warn", "Concurrent limit reached", { activeExecutions: executions.active, ip: req.ip });
    return res.status(503).json({ error: "Server is busy. Please try again in a moment." });
  }

  const source = sanitizeSource(req.body.source);
  const inputs = typeof req.body.inputs === "string" ? req.body.inputs : "";

  let tmpFile, tmpDir;
  try {
    ({ tmpFile, tmpDir } = writeTempFile(source));
  } catch (err) {
    log("error", "Failed to create temp file", { error: err.message });
    return res.status(500).json({ error: "Internal server error. Please try again." });
  }

  executions.active++;
  
  const timeout = getExecutionTimeout(source);
  log("info", "Execution started", { activeExecutions: executions.active, sourceLength: source.length, timeout, ip: req.ip });

  try {
    const { stdout, stderr, ms, exitCode } = await runCompiler(tmpFile, [], inputs, timeout);
    log("info", "Execution finished", { ms, exitCode, stdoutLength: stdout.length, activeExecutions: executions.active });
    const terminalOutput = filterStderr(stderr) || "";
    if (stdout && stdout.trim().length > 0)
      return res.json({ output: stdout, terminal: terminalOutput, ms })
    const msg = terminalOutput || "Unknown runtime error.";
    return res.json({ error: msg, terminal: terminalOutput, ms })
  } catch (e) {
    if (e.type === "timeout")
      return res.json({ error: "Execution timed out. Check for infinite loops.", ms: e.ms });
    return res.json({ error: "Unknown runtime error." });
  } finally {
    executions.active--;
    cleanupTempFile(tmpFile, tmpDir);
  }
});

// POST /compile-bytecode  { source: string } → binary .nbc download
router.post("/compile-bytecode", async (req, res) => {
  const validationError = validateSource(req.body?.source);
  if (validationError)
    return res.status(400).json({ error: validationError });

  const source = sanitizeSource(req.body.source);
  let tmpFile, tmpDir;
  try {
    ({ tmpFile, tmpDir } = writeTempFile(source));
  } catch (err) {
    return res.status(500).json({ error: "Internal server error." });
  }

  const nbcFile = tmpFile.replace(/\.csl$/, '.nbc');

  try {
    const { stderr, exitCode } = await runCompiler(
      tmpFile,
      ["--emit-bytecode", "-o", nbcFile],
      "",
      30000
    );
    if (exitCode !== 0 || !fs.existsSync(nbcFile)) {
      const msg = filterStderr(stderr) || "Compilation failed.";
      return res.status(400).json({ error: msg });
    }
    const nbcData = fs.readFileSync(nbcFile);
    res.setHeader("Content-Type", "application/octet-stream");
    res.setHeader("Content-Disposition", 'attachment; filename="program.nbc"');
    res.send(nbcData);
  } catch (e) {
    if (e.type === "timeout")
      return res.status(408).json({ error: "Compilation timed out." });
    return res.status(500).json({ error: "Unknown error during bytecode compilation." });
  } finally {
    try { if (fs.existsSync(nbcFile)) fs.unlinkSync(nbcFile); } catch (_) {}
    cleanupTempFile(tmpFile, tmpDir);
  }
});

router.post("/run-stream", async (req, res) => {
    const validationError = validateSource(req.body?.source);
    if (validationError) return res.status(400).json({ error: validationError });
    if (executions.active >= MAX_CONCURRENT) return res.status(503).json({ error: "Server busy." });

    const streamUser = req.user;

    const modelS3Key = '';

    const source = sanitizeSource(req.body.source);
    const inputs = typeof req.body.inputs === "string" ? req.body.inputs : "";
    let tmpFile, tmpDir;
    try { ({ tmpFile, tmpDir } = writeTempFile(source)); }
    catch (err) { return res.status(500).json({ error: "Internal error." }); }

    const execTimeout = getExecutionTimeout(source);
    let child;
    try { child = spawnCompiler([tmpFile], { cwd: tmpDir, timeoutMs: execTimeout }); }
    catch (err) { cleanupTempFile(tmpFile, tmpDir); return res.status(503).json({ error: err.message }); }

    res.setHeader('Content-Type', 'text/event-stream');
    // no-transform: proxies must not compress (and so buffer) the stream;
    // X-Accel-Buffering: nginx would otherwise buffer it and deliver all output at the end.
    res.setHeader('Cache-Control', 'no-cache, no-transform');
    res.setHeader('X-Accel-Buffering', 'no');
    res.setHeader('Connection', 'keep-alive');
    res.flushHeaders();

    executions.active++;
    const start = Date.now();
    const send = (type, text) => { if (!res.destroyed && !res.writableEnded) res.write(`data: ${JSON.stringify({ type, text })}\n\n`); };

    let timedOut = false;
    let stdout = '';
    let stderr = '';
    const pendingSaveFiles = []; // local paths printed via NOVA_SAVE_FILE:
    const pendingS3Files   = []; // S3 keys printed via NOVA_S3_FILE: (already uploaded by SLURM)

    let stdoutBuf = '';
    let flushTimer = null;
    const MARKERS = ['NOVA_SAVE_FILE:', 'NOVA_S3_FILE:'];
    // A partial line that is, or could still become, an internal marker line is never shown.
    const mayBeMarker = s => MARKERS.some(m => s.startsWith(m) || m.startsWith(s));
    // Send the incomplete last line, e.g. a progress bar redrawn in place with '\r'. Frames before
    // the last '\r' would be overwritten anyway, so only '\r' + the latest frame is sent. A buffer
    // ending in '\r' may be the first half of a "\r\n" split across chunks, so it waits.
    const flushPartial = () => {
        flushTimer = null;
        if (!stdoutBuf || stdoutBuf.endsWith('\r') || mayBeMarker(stdoutBuf)) return;
        const cr = stdoutBuf.lastIndexOf('\r');
        send('stdout', cr > 0 ? stdoutBuf.slice(cr) : stdoutBuf);
        stdoutBuf = '';
    };

    child.stdout.on('data', d => {
        stdoutBuf += d.toString();
        stdout   += d.toString();

        // Process complete lines, intercepting internal marker lines
        const lines = stdoutBuf.split('\n');
        stdoutBuf = lines.pop(); // keep partial last line
        const visible = [];
        for (const line of lines) {
            if (line.startsWith('NOVA_SAVE_FILE:')) {
                const file = line.slice('NOVA_SAVE_FILE:'.length).trim();
                if (pendingSaveFiles.length < 10 && !pendingSaveFiles.includes(file)) pendingSaveFiles.push(file);
            } else if (line.startsWith('NOVA_S3_FILE:')) {
                const key = line.slice('NOVA_S3_FILE:'.length).trim();
                if (pendingS3Files.length < 10 && !pendingS3Files.includes(key)) pendingS3Files.push(key);
            } else {
                visible.push(line);
            }
        }
        if (visible.length > 0) send('stdout', visible.join('\n') + '\n');
        // Throttle, don't debounce: a program printing continuously (a training loop updating a
        // progress bar) must still see its partial line within ~100 ms, not only once it goes quiet.
        if (stdoutBuf && !flushTimer) flushTimer = setTimeout(flushPartial, 100);
    });

    child.stderr.on('data', d => { stderr += d.toString(); });

    if (inputs.trim()) child.stdin.write(inputs.trim() + '\n');
    child.stdin.end();

    const timer = setTimeout(() => { timedOut = true; child.kill(); }, execTimeout);

    let finished = false;
    res.on('close', () => { if (!finished) child.kill(); });
    child.on('close', async (code) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        clearTimeout(flushTimer);
        const isMarker = stdoutBuf.startsWith('NOVA_SAVE_FILE:') ||
                         stdoutBuf.startsWith('NOVA_S3_FILE:');
        if (stdoutBuf && !isMarker) { send('stdout', stdoutBuf); stdoutBuf = ''; }
        const f = filterStderr(stderr);
        if (f) send('stderr', f);
        if (timedOut) {
            const secs = Math.round(execTimeout / 1000);
            send('error', `Execution timed out (> ${secs}s).`);
        } else if (code !== 0 && !stdout) {
            send('error', `Runtime error (exit code ${code})`);
        }

        // Upload any local files the compiler saved (NOVA_SAVE_FILE:) to user's S3 storage
        if (streamUser && pendingSaveFiles.length > 0) {
            for (const filePath of pendingSaveFiles) {
                try {
                    const absPath = security.safePath(tmpDir, filePath, { regularFile: true });
                    if (!absPath || fs.statSync(absPath).size > MAX_OUTPUT_BYTES) continue;
                    const fileName = path.basename(filePath);
                    const content  = fs.readFileSync(absPath);
                    await s3.send(new PutObjectCommand({
                        Bucket: S3_BUCKET,
                        Key: `${streamUser.userId}/${fileName}`,
                        Body: content,
                    }));
                    await dynamo.send(new PutCommand({
                        TableName: FILES_TABLE,
                        Item: {
                            userId:    streamUser.userId,
                            filePath:  fileName,
                            updatedAt: new Date().toISOString(),
                        },
                    }));
                    send('stdout', `Nova: ${fileName} uploaded to your files.\n`);
                } catch (e) {
                    log('warn', 'NOVA_SAVE_FILE upload failed', { error: e.message });
                }
            }
        }

        // Register models uploaded directly to S3 by SLURM (NOVA_S3_FILE:)
        if (streamUser && pendingS3Files.length > 0) {
            for (const s3Key of pendingS3Files) {
                if (!modelS3Key || s3Key !== modelS3Key) continue;
                try {
                    const fileName = path.basename(s3Key);
                    await dynamo.send(new PutCommand({
                        TableName: FILES_TABLE,
                        Item: {
                            userId:    streamUser.userId,
                            filePath:  fileName,
                            updatedAt: new Date().toISOString(),
                        },
                    }));
                    send('stdout', `Nova: ${fileName} saved to your file storage.\n`);
                } catch (e) {
                    log('warn', 'NOVA_S3_FILE DynamoDB registration failed', { error: e.message });
                }
            }
        }

        send('done', String(Date.now() - start));
        res.end();
        executions.active--;
        cleanupTempFile(tmpFile, tmpDir);
    });

    child.on('error', err => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        clearTimeout(flushTimer);
        send('error', err.message);
        send('done', String(Date.now() - start));
        res.end();
        executions.active--;
        cleanupTempFile(tmpFile, tmpDir);
    });
});

router.post("/compare", async (req, res) => {
  const validationError = validateSource(req.body?.source);
  if (validationError) {
    log("warn", "Invalid compare request", { reason: validationError, ip: req.ip });
    return res.status(400).json({ error: validationError });
  }

  if (executions.active + 2 > MAX_CONCURRENT) {
    log("warn", "Concurrent limit reached for compare", { activeExecutions: executions.active, ip: req.ip });
    return res.status(503).json({ error: "Server is busy. Please try again in a moment." });
  }

  const source = sanitizeSource(req.body.source);
  const inputs = typeof req.body.inputs === "string" ? req.body.inputs : "";

  let tmpFile, tmpDir;
  try {
    ({ tmpFile, tmpDir } = writeTempFile(source));
  } catch (err) {
    log("error", "Failed to create temp file for compare", { error: err.message });
    return res.status(500).json({ error: "Internal server error. Please try again." });
  }

  executions.active += 2;
  log("info", "Compare started", { activeExecutions: executions.active, sourceLength: source.length, ip: req.ip });

  try {
    const normalRun      = await runCompiler(tmpFile, [], inputs);
    const normalBench    = await runCompiler(tmpFile, ["--benchmark", "--iters", String(BENCH_ITERS)], inputs);
    const optimizedRun   = await runCompiler(tmpFile, ["--optimize"], inputs);
    const optimizedBench = await runCompiler(tmpFile, ["--optimize", "--benchmark", "--iters", String(BENCH_ITERS)], inputs);

    const normalStats    = parseBenchStats(normalBench.stdout,    normalBench.stderr);
    const optimizedStats = parseBenchStats(optimizedBench.stdout, optimizedBench.stderr);
    console.log("NORMAL:", JSON.stringify(normalRun.stdout));
    console.log("OPTIMIZED:", JSON.stringify(optimizedRun.stdout));
    const normalize = s => s.trim().replace(/\r\n/g, '\n');
    const outputMismatch = normalize(normalRun.stdout) !== normalize(optimizedRun.stdout);

    log("info", "Compare finished", {
      normalUs: normalStats.us, optimizedUs: optimizedStats.us,
      normalNodes: normalStats.nodes, optNodes: optimizedStats.nodes,
    });

    return res.json({
      output: normalRun.stdout || "(no output)",
      outputMismatch,
      normal:    { us: normalStats.us, nodes: normalStats.nodes, iters: normalStats.iters },
      optimized: { us: optimizedStats.us, nodes: optimizedStats.nodes, iters: optimizedStats.iters, folds: optimizedStats.folds, removed: optimizedStats.removed },
      speedup: normalStats.us && optimizedStats.us ? (normalStats.us / optimizedStats.us).toFixed(2) : null,
      nodesEliminated: (normalStats.nodes ?? 0) - (optimizedStats.nodes ?? 0),
    });
  } catch (e) {
    if (e.type === "timeout")
      return res.json({ error: "Execution timed out (> 7 seconds). Check for infinite loops." });
    log("error", "Compare failed", { error: e.message });
    return res.json({ error: "Comparison failed. Please try again." });
  } finally {
    executions.active -= 2;
    cleanupTempFile(tmpFile, tmpDir);
  }
});

module.exports = router;
