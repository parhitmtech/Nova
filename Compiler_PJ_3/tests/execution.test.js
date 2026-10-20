const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');

// Real processes prove that execution limits and environment isolation work.
test('local execution drops backend secrets and caps runaway output', async () => {
  const { createExecutor } = require('../execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-runner-test-'));
  const executable = path.join(dir, 'fixture');
  fs.writeFileSync(executable, `#!${process.execPath}\nconsole.log(process.env.JWT_SECRET || 'no-secret');\nsetInterval(() => process.stdout.write('x'.repeat(4096)), 5);\n`, { mode: 0o755 });
  const executor = createExecutor({ mode: 'local', nodeEnv: 'test', binary: executable, maxOutputBytes: 8192 });
  process.env.JWT_SECRET = 'test-backend-secret-never-forward';
  try {
    const child = executor.spawnCompiler([], { cwd: dir, timeoutMs: 2000 });
    let output = '';
    child.stdout.on('data', chunk => { output += chunk; });
    const [code] = await once(child, 'close');
    assert.notEqual(code, 0);
    assert.equal(child.stopReason, 'output_limit');
    assert.ok(Buffer.byteLength(output) <= 8192);
    assert.ok(output.startsWith('no-secret'));
    assert.ok(!output.includes('test-backend-secret'));
  } finally { delete process.env.JWT_SECRET; fs.rmSync(dir, { recursive: true, force: true }); }
});

test('runner stops a hung program and releases the execution slot', async () => {
  const { createExecutor } = require('../execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-timeout-test-'));
  const executable = path.join(dir, 'fixture');
  fs.writeFileSync(executable, `#!${process.execPath}\nsetInterval(() => {}, 1000);\n`, { mode: 0o755 });
  try {
    const executor = createExecutor({ mode: 'local', nodeEnv: 'test', binary: executable, maxConcurrent: 1 });
    const child = executor.spawnCompiler([], { cwd: dir, timeoutMs: 100 });
    child.stderr.resume();
    assert.throws(() => executor.spawnCompiler([], { cwd: dir }), /busy/i);
    await once(child, 'close');
    assert.equal(child.stopReason, 'timeout');
    const next = executor.spawnCompiler([], { cwd: dir, timeoutMs: 100 });
    next.stderr.resume();
    await once(next, 'close');
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('production refuses local execution and arguments outside the job directory', () => {
  const { createExecutor } = require('../execution');
  assert.throws(() => createExecutor({ mode: 'local', nodeEnv: 'production' }), /production/i);
  const executor = createExecutor({ mode: 'docker', nodeEnv: 'production' });
  assert.throws(() => executor.spawnCompiler(['/etc/passwd'], { cwd: os.tmpdir() }), /job directory/i);
  assert.throws(() => executor.spawnCompiler(['--emit-bytecode', '-o', '/etc/evil'], { cwd: os.tmpdir() }), /job directory/i);
});

test('docker jobs have resource limits and no credentials, host network, or root privileges', () => {
  const { createExecutor } = require('../execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-spec-test-'));
  try {
    const executor = createExecutor({ mode: 'docker', nodeEnv: 'production' });
    const spec = executor.buildCommand([], { cwd: dir });
    assert.equal(spec.command, 'docker');
    assert.ok(spec.args.includes('--network=none'));
    assert.ok(spec.args.includes('--read-only'));
    assert.ok(spec.args.includes('--cap-drop=ALL'));
    assert.ok(spec.args.includes('--security-opt=no-new-privileges'));
    assert.ok(spec.args.includes('--memory=256m'));
    assert.ok(spec.args.includes('--pids-limit=32'));
    assert.ok(spec.args.includes('--cpus=1'));
    assert.ok(spec.args.includes('--user'));
    assert.notEqual(spec.args[spec.args.indexOf('--user') + 1].split(':')[0], '0');
    assert.equal(spec.env.JWT_SECRET, undefined);
    assert.equal(spec.env.AWS_ACCESS_KEY_ID, undefined);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('Docker spec permits files above 16 KiB, enforces an internal deadline, and preserves client configuration', () => {
  const { createExecutor } = require('../execution');
  const prior = process.env.DOCKER_HOST;
  process.env.DOCKER_HOST = 'tcp://worker.example:2376';
  try {
    const spec = createExecutor({ mode: 'docker' }).buildCommand([], { cwd: os.tmpdir(), timeoutMs: 1200, userId: 'alice' });
    assert.ok(spec.args.includes('fsize=16777216:16777216'));
    assert.ok(spec.args.includes('/usr/bin/timeout'));
    assert.ok(spec.args.includes('1.2s'));
    assert.ok(spec.args.includes('--label'));
    assert.equal(spec.env.DOCKER_HOST, process.env.DOCKER_HOST);
  } finally { prior === undefined ? delete process.env.DOCKER_HOST : process.env.DOCKER_HOST = prior; }
});

test('per-user batch quotas and the separate REPL pool preserve execution for other users', async () => {
  const { createExecutor } = require('../execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-quota-'));
  const fixture = path.join(dir, 'fixture');
  fs.writeFileSync(fixture, `#!${process.execPath}\nsetInterval(() => {}, 1000);\n`, { mode: 0o755 });
  const executor = createExecutor({ mode: 'local', nodeEnv: 'test', binary: fixture });
  const children = [];
  const start = (userId, kind = 'batch') => {
    const child = executor.spawnCompiler([], { cwd: dir, userId, kind, timeoutMs: 5000 });
    child.stdout.resume(); child.stderr.resume(); children.push(child); return child;
  };
  try {
    start('alice', 'repl');
    assert.throws(() => start('alice', 'repl'), e => e.type === 'busy' && e.status === 503);
    start('bob', 'repl');
    assert.throws(() => start('carol', 'repl'), e => e.type === 'busy' && e.status === 503);
    start('alice'); start('alice');
    assert.throws(() => start('alice'), e => e.type === 'busy' && e.status === 503);
    start('bob'); start('bob'); start('carol');
    assert.throws(() => start('dave'), /busy/i);
  } finally {
    const closed = children.map(child => once(child, 'close'));
    children.forEach(child => child.kill()); await Promise.all(closed);
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('startup sweep targets only this worker labels and passes Docker client env through cleanup', async () => {
  const { sweepContainers } = require('../execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-docker-client-'));
  const log = path.join(dir, 'calls.jsonl');
  const previous = { PATH: process.env.PATH, DOCKER_HOST: process.env.DOCKER_HOST, DOCKER_CONFIG: process.env.DOCKER_CONFIG, NOVA_WORKER_ID: process.env.NOVA_WORKER_ID, NOVA_EXECUTION_MODE: process.env.NOVA_EXECUTION_MODE };
  fs.writeFileSync(path.join(dir, 'docker'), `#!${process.execPath}
const fs = require('fs');
fs.appendFileSync(${JSON.stringify(log)}, JSON.stringify({ args: process.argv.slice(2), host: process.env.DOCKER_HOST, config: process.env.DOCKER_CONFIG, secret: process.env.JWT_SECRET }) + '\\n');
if (process.argv[2] === 'ps') console.log('abcdef123456');
`, { mode: 0o755 });
  Object.assign(process.env, { PATH: dir + path.delimiter + previous.PATH, DOCKER_HOST: 'tcp://fixture:2376', DOCKER_CONFIG: dir, NOVA_WORKER_ID: 'review-worker', NOVA_EXECUTION_MODE: 'docker' });
  try {
    await sweepContainers();
    const calls = fs.readFileSync(log, 'utf8').trim().split('\n').map(line => JSON.parse(line));
    assert.deepEqual(calls[0].args, ['ps', '-aq', '--filter', 'label=nova.job=true', '--filter', 'label=nova.worker=review-worker']);
    assert.deepEqual(calls[1].args, ['rm', '-f', 'abcdef123456']);
    for (const call of calls) {
      assert.equal(call.host, 'tcp://fixture:2376'); assert.equal(call.config, dir); assert.equal(call.secret, undefined);
    }
    fs.writeFileSync(path.join(dir, 'docker'), `#!${process.execPath}\nprocess.exit(1);\n`, { mode: 0o755 });
    await assert.rejects(sweepContainers());
  } finally {
    for (const [key, value] of Object.entries(previous)) value === undefined ? delete process.env[key] : process.env[key] = value;
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
