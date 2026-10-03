'use strict';

const { spawn } = require('node:child_process');
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
  let active = 0;

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
    const uid = process.getuid?.() || 65534;
    const gid = process.getgid?.() || 65534;
    const name = 'nova-job-' + crypto.randomUUID();
    return {
      command: 'docker', cwd, env, containerName: name,
      args: ['run', '--rm', '--init', '-i', '--name', name,
        '--network=none', '--read-only', '--cap-drop=ALL', '--security-opt=no-new-privileges',
        '--memory=256m', '--memory-swap=256m', '--pids-limit=32', '--cpus=1',
        '--ulimit', 'nofile=64:64', '--ulimit', 'fsize=16384:16384',
        '--user', `${uid}:${gid}`, '--tmpfs', '/tmp:rw,noexec,nosuid,size=32m',
        '--mount', `type=bind,source=${cwd},target=${cwd}`, '--workdir', cwd,
        '--env', 'NOVA_HOME=/opt/nova', process.env.NOVA_WORKER_IMAGE || 'novacomp-worker:local', ...args],
    };
  }

  function spawnCompiler(args, options = {}) {
    if (active >= maxConcurrent) throw new Error('Execution server is busy. Try again shortly.');
    const spec = buildCommand(args, options);
    const child = spawn(spec.command, spec.args, {
      cwd: spec.cwd, env: spec.env, shell: false, windowsHide: true,
      detached: process.platform !== 'win32', stdio: ['pipe', 'pipe', 'pipe'],
    });
    active++;
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
    const timeout = Math.min(Math.max(Number(options.timeoutMs) || 30000, 1), 300000);
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
      active--;
      clearTimeout(timer);
      result.stdout.end();
      result.stderr.end();
      result.exitCode = result.stopReason ? 1 : code;
      result.emit('close', result.exitCode, signal);
    });
    return result;
  }
  return { spawnCompiler, buildCommand };
}

let defaultExecutor;
function spawnCompiler(args, options) {
  defaultExecutor ||= createExecutor();
  return defaultExecutor.spawnCompiler(args, options);
}
module.exports = { createExecutor, spawnCompiler };
