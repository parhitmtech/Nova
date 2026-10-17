'use strict';
// SOL cluster access over the reverse SSH tunnel (SSH exec, SFTP) and GPU workload detection.
const fs = require('fs');

// ssh2 / ssh2-sftp-client are optional — install with: npm install ssh2 ssh2-sftp-client
// Required for GPU job submission via the SOL reverse tunnel.
let SshClient = null;
let SftpClient = null;
try {
  SshClient  = require('ssh2').Client;
  SftpClient = require('ssh2-sftp-client');
} catch (_) {}

// ── GPU / SLURM helpers ───────────────────────────────────────────────────────

const SSH_TIMEOUT_MS = 30_000;

function isTunnelError(err) {
  const msg = err?.message || '';
  return msg.includes('connect') || msg.includes('refused') || msg.includes('ECONNREFUSED')
      || msg.includes('ETIMEDOUT') || msg.includes('Timed out');
}

// Detect what kind of GPU workload the source contains.
// Returns: 'cuda' | 'hf_sol' | 'hf' | 'sol' | 'none'
function detectGpuScenario(source) {
  const hasTensors = /\btensor\b/.test(source);
  const hasHF      = /\bimport\s+hf\b/.test(source);
  const hasSOL     = /\bimport\s+sol\b/.test(source);
  if (hasTensors)       return 'cuda';
  if (hasHF && hasSOL)  return 'hf_sol';
  if (hasHF)            return 'hf';
  if (hasSOL)           return 'sol';
  return 'none';
}

const GPU_BENEFIT_MSG = {
  cuda:   'CUDA parallelism dramatically accelerates tensor operations over CPU.',
  hf:     'HuggingFace inference API uses GPU-accelerated servers for model inference.',
  sol:    'SOL HPC cluster runs your job on dedicated GPU nodes via SLURM.',
  hf_sol: 'SOL provides GPU nodes to fine-tune HuggingFace models at scale.',
};

// Execute a command on SOL via SSH and return { stdout, stderr }.
// Rejects after SSH_TIMEOUT_MS if the connection cannot be established.
function sshExec(command) {
  return new Promise((resolve, reject) => {
    if (!SshClient) return reject(new Error('ssh2 module not installed'));

    const conn = new SshClient();
    const keyPath = process.env.SOL_PRIVATE_KEY_PATH;

    let settled = false;
    const fail = (err) => { if (!settled) { settled = true; conn.end(); reject(err); } };

    const timer = setTimeout(() => fail(new Error('Timed out waiting for SSH connection')), SSH_TIMEOUT_MS);

    conn.on('ready', () => {
      clearTimeout(timer);
      conn.exec(command, (err, stream) => {
        if (err) return fail(err);
        let stdout = '', stderr = '';
        stream.on('data', d => { stdout += d.toString(); });
        stream.stderr.on('data', d => { stderr += d.toString(); });
        stream.on('close', () => {
          settled = true;
          conn.end();
          resolve({ stdout, stderr });
        });
      });
    });

    conn.on('error', fail);

    conn.connect({
      host:         process.env.SOL_HOST     || 'localhost',
      port:         parseInt(process.env.SOL_PORT || '2222', 10),
      username:     process.env.SOL_USERNAME,
      privateKey:   fs.readFileSync(keyPath),
      readyTimeout: SSH_TIMEOUT_MS,
    });
  });
}

// Upload a local file to SOL via SFTP.
async function sftpUpload(localPath, remotePath) {
  if (!SftpClient) throw new Error('ssh2-sftp-client module not installed');
  const sftp = new SftpClient();
  try {
    await sftp.connect({
      host:         process.env.SOL_HOST     || 'localhost',
      port:         parseInt(process.env.SOL_PORT || '2222', 10),
      username:     process.env.SOL_USERNAME,
      privateKey:   fs.readFileSync(process.env.SOL_PRIVATE_KEY_PATH),
      readyTimeout: SSH_TIMEOUT_MS,
    });
    await sftp.put(localPath, remotePath);
  } finally {
    await sftp.end().catch(() => {});
  }
}

// Download a remote file from SOL via SFTP to a local path.
async function sftpDownload(remotePath, localPath) {
  if (!SftpClient) throw new Error('ssh2-sftp-client module not installed');
  const sftp = new SftpClient();
  try {
    await sftp.connect({
      host:         process.env.SOL_HOST     || 'localhost',
      port:         parseInt(process.env.SOL_PORT || '2222', 10),
      username:     process.env.SOL_USERNAME,
      privateKey:   fs.readFileSync(process.env.SOL_PRIVATE_KEY_PATH),
      readyTimeout: SSH_TIMEOUT_MS,
    });
    await sftp.get(remotePath, localPath);
  } finally {
    await sftp.end().catch(() => {});
  }
}

module.exports = { isTunnelError, detectGpuScenario, GPU_BENEFIT_MSG, sshExec, sftpUpload, sftpDownload };
