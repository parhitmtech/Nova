'use strict';
// GPU jobs on SOL: /gpu/submit, /gpu/status/:jobId, /gpu/results/:jobId.
const express = require('express');
const fs = require('fs');
const path = require('path');
const { MAX_EXECUTION_MS, ML_EXECUTION_MS, MAX_CONCURRENT } = require('../config');
const { log } = require('../lib/logger');
const { filterStderr, sanitizeSource, validateSource, LOCAL_TEMP, writeTempFile, cleanupTempFile, runCompiler } = require('../services/compiler-runner');
const { isTunnelError, detectGpuScenario, GPU_BENEFIT_MSG, sshExec, sftpUpload, sftpDownload } = require('../services/sol');

const router = express.Router();

// ── POST /gpu/submit ──────────────────────────────────────────────────────────

router.post('/gpu/submit', async (req, res) => {
  let userJobs = 0, totalJobs = 0;
  for (const entry of fs.readdirSync(LOCAL_TEMP, { withFileTypes: true })) {
    if (!entry.isFile() || !/^gpu_job_[0-9]+_[a-z0-9]+\.json$/.test(entry.name)) continue;
    try {
      const job = JSON.parse(fs.readFileSync(path.join(LOCAL_TEMP, entry.name), 'utf8'));
      if (['pending', 'running'].includes(job.status)) { totalJobs++; if (job.ownerId === req.user.userId) userJobs++; }
    } catch (_) {}
  }
  if (userJobs >= 2 || totalJobs >= MAX_CONCURRENT) return res.status(503).json({ error: 'GPU job limit reached' });
  const validationError = validateSource(req.body?.source);
  if (validationError) return res.status(400).json({ error: validationError });

  const source   = sanitizeSource(req.body.source);
  const scenario = detectGpuScenario(source);

  if (scenario === 'none') {
    return res.status(400).json({
      error: [
        'No GPU-compatible operations detected. Run on GPU supports:',
        '  • tensor T[N]  — CUDA codegen compiled and run on SOL GPU nodes',
        '  • import hf    — HuggingFace inference (GPU-accelerated remote API)',
        '  • import sol   — SOL HPC cluster (dedicated GPU nodes via SLURM)',
      ].join('\n'),
    });
  }

  const jobId = Date.now() + '_' + Math.random().toString(36).slice(2);
  const metaPath = path.join(LOCAL_TEMP, `gpu_job_${jobId}.json`);

  let tmpFile, tmpDir;
  try {
    ({ tmpFile, tmpDir } = writeTempFile(source));
  } catch (err) {
    return res.status(500).json({ error: 'Internal server error.' });
  }

  // ── CUDA scenario: compile Nova → .cu → upload to SOL → sbatch ───────────
  if (scenario === 'cuda') {
    try {
      const cuFile = tmpFile.replace('.csl', '.cu');
      await runCompiler(tmpFile, ['--emit-cuda'], '', MAX_EXECUTION_MS, req.user.userId);

      if (!fs.existsSync(cuFile)) {
        return res.status(400).json({
          error: 'CUDA codegen failed. Verify that tensor declarations and operations are correct.',
        });
      }

      const jobDir = `${process.env.SOL_WORK_DIR}/${jobId}`;
      await sshExec(`mkdir -p ${jobDir}`);
      await sftpUpload(cuFile, `${jobDir}/program.cu`);

      const slurmScript = `#!/bin/bash
#SBATCH --job-name=novacomp_${jobId}
#SBATCH --output=${jobDir}/output.txt
#SBATCH --error=${jobDir}/error.txt
#SBATCH --time=00:10:00
#SBATCH --partition=general
#SBATCH --gres=gpu:1
#SBATCH --mem=4G
#SBATCH --nodes=1
#SBATCH --ntasks=1

cd ${jobDir}
module load cuda/12.9
nvcc -O2 -o program program.cu
./program
echo "EXIT_CODE:$?"
`;
      const slurmFile = path.join(tmpDir, 'job.slurm');
      fs.writeFileSync(slurmFile, slurmScript, 'utf8');
      await sftpUpload(slurmFile, `${jobDir}/job.slurm`);

      const { stdout: sbatchOut, stderr: sbatchErr } = await sshExec(`sbatch ${jobDir}/job.slurm`);
      const slurmJobId = sbatchOut.trim().split(/\s+/).pop();
      if (!slurmJobId || isNaN(parseInt(slurmJobId, 10))) {
        log('error', 'sbatch returned unexpected output', { sbatchOut, sbatchErr });
        return res.status(500).json({ error: 'SLURM submission failed: ' + (sbatchErr || sbatchOut) });
      }

      const jobMeta = {
        ownerId: req.user.userId, jobId, slurmJobId, type: 'slurm', scenario,
        clientIp: req.ip, submittedAt: new Date().toISOString(),
        status: 'pending', jobDir,
      };
      fs.writeFileSync(metaPath, JSON.stringify(jobMeta), 'utf8');

      log('info', 'GPU CUDA job submitted', { jobId, slurmJobId, ip: req.ip });
      return res.json({ jobId, slurmJobId, status: 'pending', scenario, gpuBenefit: GPU_BENEFIT_MSG.cuda });

    } catch (err) {
      log('error', 'GPU CUDA submission failed', { error: err.message });
      if (isTunnelError(err))
        return res.status(503).json({ error: 'GPU service unavailable. The SOL tunnel may be offline.' });
      return res.status(500).json({ error: 'Job submission failed: ' + err.message });
    } finally {
      cleanupTempFile(tmpFile, tmpDir);
    }
  }

  // ── HF / SOL scenarios: run compiler locally, output stored in job file ───
  // The compiler makes HuggingFace API calls and/or SSHes into SOL internally.
  // We fire it off in the background so the response returns immediately.
  const jobMeta = {
    ownerId: req.user.userId, jobId, type: 'local', scenario,
    clientIp: req.ip, submittedAt: new Date().toISOString(),
    status: 'running', output: null, errorOutput: null,
  };
  fs.writeFileSync(metaPath, JSON.stringify(jobMeta), 'utf8');

  log('info', 'GPU local job started', { jobId, scenario, ip: req.ip });
  res.json({ jobId, status: 'running', scenario, gpuBenefit: GPU_BENEFIT_MSG[scenario] });

  // Run compiler in background — fire and forget
  const execTimeout = /\/\/\s*@ml\b/.test(source) ? ML_EXECUTION_MS : MAX_EXECUTION_MS;
  runCompiler(tmpFile, [], '', execTimeout, req.user.userId)
    .then(({ stdout, stderr, exitCode }) => {
      jobMeta.status      = (exitCode === 0 || stdout.trim().length > 0) ? 'completed' : 'failed';
      jobMeta.output      = stdout;
      jobMeta.errorOutput = filterStderr(stderr);
      fs.writeFileSync(metaPath, JSON.stringify(jobMeta), 'utf8');
      log('info', 'GPU local job finished', { jobId, scenario, exitCode });
    })
    .catch(err => {
      jobMeta.status      = 'failed';
      jobMeta.errorOutput = err.message || 'Execution failed';
      fs.writeFileSync(metaPath, JSON.stringify(jobMeta), 'utf8');
      log('error', 'GPU local job error', { jobId, scenario, error: err.message });
    })
    .finally(() => { cleanupTempFile(tmpFile, tmpDir); });
});

// ── GET /gpu/status/:jobId ────────────────────────────────────────────────────

router.get('/gpu/status/:jobId', async (req, res) => {
  const { jobId } = req.params;
  if (!/^[0-9]+_[a-z0-9]+$/.test(jobId)) return res.status(400).json({ error: 'Invalid job ID' });
  const metaPath  = path.join(LOCAL_TEMP, `gpu_job_${jobId}.json`);

  if (!fs.existsSync(metaPath))
    return res.status(404).json({ error: 'Job not found' });

  let jobMeta;
  try { jobMeta = JSON.parse(fs.readFileSync(metaPath, 'utf8')); }
  catch { return res.status(500).json({ error: 'Failed to read job metadata' }); }
  if (jobMeta.ownerId !== req.user.userId) return res.status(404).json({ error: 'Job not found' });

  // If already in a terminal state, return immediately without re-querying SLURM
  if (jobMeta.status === 'completed' || jobMeta.status === 'failed') {
    return res.json({ jobId, slurmJobId: jobMeta.slurmJobId, status: jobMeta.status, scenario: jobMeta.scenario });
  }

  // Local jobs (HF / SOL) update their own status file — no SLURM polling needed
  if (jobMeta.type === 'local') {
    return res.json({ jobId, status: jobMeta.status, scenario: jobMeta.scenario });
  }

  try {
    // squeue exits non-zero and prints nothing when job is no longer queued
    const { stdout } = await sshExec(
      `squeue -j ${jobMeta.slurmJobId} -h -o "%T" 2>/dev/null || echo "GONE"`
    );
    const slurmStatus = stdout.trim();

    let status;
    if (slurmStatus === 'RUNNING')            status = 'running';
    else if (slurmStatus === 'PENDING')       status = 'pending';
    else if (slurmStatus === 'FAILED' || slurmStatus === 'CANCELLED') status = 'failed';
    else {
      // GONE or COMPLETED — check whether output file landed
      const { stdout: probe } = await sshExec(
        `test -f ${jobMeta.jobDir}/output.txt && echo EXISTS || echo MISSING`
      );
      status = probe.includes('EXISTS') ? 'completed' : 'failed';
    }

    jobMeta.status = status;
    fs.writeFileSync(metaPath, JSON.stringify(jobMeta), 'utf8');

    res.json({ jobId, slurmJobId: jobMeta.slurmJobId, status });

  } catch (err) {
    if (isTunnelError(err))
      return res.status(503).json({ error: 'GPU service unavailable.' });
    res.status(500).json({ error: err.message });
  }
});

// ── GET /gpu/results/:jobId ───────────────────────────────────────────────────

router.get('/gpu/results/:jobId', async (req, res) => {
  const { jobId } = req.params;
  if (!/^[0-9]+_[a-z0-9]+$/.test(jobId)) return res.status(400).json({ error: 'Invalid job ID' });
  const metaPath  = path.join(LOCAL_TEMP, `gpu_job_${jobId}.json`);

  if (!fs.existsSync(metaPath))
    return res.status(404).json({ error: 'Job not found' });

  let jobMeta;
  try { jobMeta = JSON.parse(fs.readFileSync(metaPath, 'utf8')); }
  catch { return res.status(500).json({ error: 'Failed to read job metadata' }); }
  if (jobMeta.ownerId !== req.user.userId) return res.status(404).json({ error: 'Job not found' });

  // Local jobs (HF / SOL) store output directly in the meta file
  if (jobMeta.type === 'local') {
    return res.json({
      jobId,
      output:      jobMeta.output      ?? '',
      error:       jobMeta.errorOutput ?? '',
      completedAt: new Date().toISOString(),
      scenario:    jobMeta.scenario,
    });
  }

  try {
    const localOutput = path.join(LOCAL_TEMP, `gpu_output_${jobId}.txt`);
    await sftpDownload(`${jobMeta.jobDir}/output.txt`, localOutput);
    const output = fs.readFileSync(localOutput, 'utf8');
    try { fs.unlinkSync(localOutput); } catch (_) {}

    let errorOutput = '';
    try {
      const localError = path.join(LOCAL_TEMP, `gpu_error_${jobId}.txt`);
      await sftpDownload(`${jobMeta.jobDir}/error.txt`, localError);
      errorOutput = fs.readFileSync(localError, 'utf8');
      try { fs.unlinkSync(localError); } catch (_) {}
    } catch (_) {}

    log('info', 'GPU results fetched', { jobId, ip: req.ip });
    res.json({ jobId, output, error: errorOutput, completedAt: new Date().toISOString() });

  } catch (err) {
    if (isTunnelError(err))
      return res.status(503).json({ error: 'GPU service unavailable.' });
    res.status(500).json({ error: 'Failed to fetch results: ' + err.message });
  }
});

module.exports = router;
