'use strict';

const { spawn, execFile } = require('node:child_process');
const { promisify } = require('node:util');
const execFileAsync = promisify(execFile);
const { EventEmitter } = require('node:events');
const { PassThrough } = require('node:stream');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

// One boundary for every web-triggered compiler process. Docker is the default;
// direct execution is an explicit, trusted-development-only escape hatch.
function createExecutor(config = {}) {
  const mode = config.mode || process.env.NOVA_EXECUTION_MODE || 'docker';
  const nodeEnv = config.nodeEnv || process.env.NODE_ENV || 'development';
  if (!['docker', 'local'].includes(mode)) throw new Error('Unknown Nova execution mode.');
  if (mode === 'local' && nodeEnv === 'production')
    throw new Error('Local execution is forbidden in production. Use the isolated worker.');
  const binary = config.binary || path.join(__dirname, process.platform === 'win32' ? 'compiler.exe' : 'compiler');
  const maxConcurrent = config.maxConcurrent || 5;
  const maxOutputBytes = config.maxOutputBytes || 256_000;
  const maxPerUser = config.maxPerUser || 2;
  const maxRepl = config.maxRepl || 2;
  const active = { batch: 0, repl: 0 };
  const byUser = new Map();
  function deadline(options) {
    return Math.min(Math.max(Number(options.timeoutMs) || 30000, 1), options.kind === 'repl' ? 1800000 : 300000);
  }

  function buildCommand(args, options = {}) {
    const cwd = path.resolve(options.cwd || '');
    if (!options.cwd || !fs.statSync(cwd).isDirectory()) throw new Error('An isolated job directory is required.');
    const root = fs.realpathSync(cwd);
    const flags = new Set(['--repl', '--optimize', '--benchmark', '--dump-ir', '--emit-bytecode', '--emit-cuda', '--debug']);
    for (let i = 0; i < args.length; i++) {
      const arg = args[i];
      if (typeof arg !== 'string' || arg.includes('\0')) throw new Error('Invalid compiler argument.');
      if (flags.has(arg)) continue;
      if (arg === '--iters') {
        if (!/^\d+$/.test(args[++i] || '') || Number(args[i]) > 10000) throw new Error('Invalid benchmark iteration count.');
        continue;
      }
      if (arg === '-o') {
        if (!args[i + 1]) throw new Error('Missing output file.');
        continue;
      }
      if (arg.startsWith('-')) throw new Error('Unsupported compiler option.');
      const target = path.resolve(cwd, arg);
      if (!target.startsWith(cwd + path.sep)) throw new Error('Compiler files must stay in the job directory.');
      let ancestor = target;
      while (!fs.existsSync(ancestor)) ancestor = path.dirname(ancestor);
      const real = fs.realpathSync(ancestor);
      if (real !== root && !real.startsWith(root + path.sep)) throw new Error('Compiler files must stay in the job directory.');
    }
    const env = { PATH: process.env.PATH || '/usr/local/bin:/usr/bin:/bin', LANG: 'C.UTF-8', NOVA_HOME: __dirname };
    if (mode === 'local') return { command: binary, args, cwd, env };
    Object.assign(env, dockerEnvironment());
    const uid = process.getuid?.() || 65534;
    const gid = process.getgid?.() || 65534;
    const name = 'nova-job-' + crypto.randomUUID();
    return {
      command: 'docker', cwd, env, containerName: name,
      args: ['run', '--rm', '--init', '-i', '--name', name,
        '--label', `nova.worker=${workerId()}`, '--label', 'nova.job=true',
        '--network=none', '--read-only', '--cap-drop=ALL', '--security-opt=no-new-privileges',
        '--memory=256m', '--memory-swap=256m', '--pids-limit=32', '--cpus=1',
        '--ulimit', 'nofile=64:64', '--ulimit', 'fsize=16777216:16777216',
        '--user', `${uid}:${gid}`, '--tmpfs', '/tmp:rw,noexec,nosuid,size=32m',
        '--mount', `type=bind,source=${cwd},target=${cwd}`, '--workdir', cwd,
        '--env', 'NOVA_HOME=/opt/nova', '--entrypoint', '/usr/bin/timeout',
        process.env.NOVA_WORKER_IMAGE || 'novacomp-worker:local',
        '--signal=KILL', `${deadline(options) / 1000}s`, '/opt/nova/compiler', ...args],
    };
  }

  function spawnCompiler(args, options = {}) {
    const kind = options.kind === 'repl' ? 'repl' : 'batch';
    const userId = options.userId || 'internal';
    const key = `${kind}:${userId}`;
    if (active[kind] >= (kind === 'repl' ? maxRepl : maxConcurrent) || (byUser.get(key) || 0) >= (kind === 'repl' ? 1 : maxPerUser))
      throw Object.assign(new Error('Execution server is busy. Try again shortly.'), { type: 'busy', status: 503 });
    const spec = buildCommand(args, options);
    const child = spawn(spec.command, spec.args, {
      cwd: spec.cwd, env: spec.env, shell: false, windowsHide: true,
      detached: process.platform !== 'win32', stdio: ['pipe', 'pipe', 'pipe'],
    });
    active[kind]++;
    byUser.set(key, (byUser.get(key) || 0) + 1);
    const result = new EventEmitter();
    result.stdout = new PassThrough();
    result.stderr = new PassThrough();
    result.stdin = child.stdin;
    result.pid = child.pid;
    result.stopReason = null;
    let outputBytes = 0;
    let finished = false;
    let stopping = false;

    function stop(reason) {
      if (finished || stopping) return;
      stopping = true;
      result.stopReason = reason;
      if (reason === 'timeout') result.stderr.write('\nExecution timed out.\n');
      if (reason === 'output_limit') result.stderr.write('\nOutput limit exceeded.\n');
      if (spec.containerName) {
        // Killing the client alone can leave the container alive.
        const removal = spawn('docker', ['rm', '-f', spec.containerName], { env: spec.env, stdio: 'ignore' });
        removal.on('error', () => {});
      }
      try {
        if (process.platform !== 'win32' && child.pid) process.kill(-child.pid, 'SIGKILL');
        else child.kill('SIGKILL');
      } catch { child.kill('SIGKILL'); }
    }
    result.kill = () => { stop('cancelled'); return true; };
    const timeout = deadline(options);
    const timer = setTimeout(() => stop('timeout'), timeout);
    timer.unref();
    child.stdin.on('error', () => {}); // A stopped job can close stdin before the caller finishes writing.
    function forward(stream, chunk) {
      if (stopping) return;
      const remaining = maxOutputBytes - outputBytes;
      const accepted = chunk.subarray(0, Math.max(0, remaining));
      outputBytes += accepted.length;
      if (accepted.length) stream.write(accepted);
      if (chunk.length > remaining) stop('output_limit');
    }
    child.stdout.on('data', chunk => forward(result.stdout, chunk));
    child.stderr.on('data', chunk => forward(result.stderr, chunk));
    child.on('error', error => {
      result.stderr.write('Could not start the execution worker. Check the worker installation.\n');
      result.emit('error', error);
    });
    child.on('close', (code, signal) => {
      if (finished) return;
      finished = true;
      active[kind]--;
      const remaining = (byUser.get(key) || 1) - 1;
      if (remaining) byUser.set(key, remaining); else byUser.delete(key);
      clearTimeout(timer);
      result.stdout.end();
      result.stderr.end();
      if (spec.containerName && code === 137 && !result.stopReason) result.stopReason = 'timeout';
      result.exitCode = result.stopReason ? 1 : code;
      result.emit('close', result.exitCode, signal);
    });
    return result;
  }
  return { spawnCompiler, buildCommand };
}

// These configure only the host Docker CLI, never the compiler container.
function dockerEnvironment() {
  const env = { PATH: process.env.PATH || '/usr/local/bin:/usr/bin:/bin' };
  for (const key of ['DOCKER_HOST', 'DOCKER_CONFIG', 'DOCKER_CONTEXT', 'DOCKER_TLS_VERIFY', 'DOCKER_CERT_PATH', 'DOCKER_API_VERSION', 'SSH_AUTH_SOCK', 'HOME'])
    if (process.env[key]) env[key] = process.env[key];
  return env;
}
function workerId() {
  const id = process.env.NOVA_WORKER_ID || 'novacomp';
  if (!/^[a-zA-Z0-9_.-]{1,64}$/.test(id)) throw new Error('Invalid NOVA_WORKER_ID');
  return id;
}
async function sweepContainers() {
  if ((process.env.NOVA_EXECUTION_MODE || 'docker') !== 'docker') return;
  const options = { env: dockerEnvironment(), timeout: 15000, maxBuffer: 1024 * 1024 };
  const { stdout } = await execFileAsync('docker', ['ps', '-aq', '--filter', 'label=nova.job=true', '--filter', `label=nova.worker=${workerId()}`], options);
  const ids = stdout.trim().split(/\s+/).filter(Boolean);
  if (ids.some(id => !/^[a-f0-9]{12,64}$/.test(id))) throw new Error('Invalid container ID from Docker');
  // Bound argv size even after repeated server crashes.
  for (let i = 0; i < ids.length; i += 100) await execFileAsync('docker', ['rm', '-f', ...ids.slice(i, i + 100)], options);
}

let defaultExecutor;
function spawnCompiler(args, options) {
  defaultExecutor ||= createExecutor();
  return defaultExecutor.spawnCompiler(args, options);
}
module.exports = { createExecutor, spawnCompiler, sweepContainers };
