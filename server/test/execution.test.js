const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');

// These tests put a stand-in `docker` script first on PATH. Windows cannot spawn a
// shebang script, so there they would reach a real Docker CLI instead: skip them.
const STAND_IN_DOCKER_SKIP = process.platform === 'win32' ? 'stand-in docker CLI needs shebang support (runs on Linux CI)' : false;

// Real processes prove that execution limits and environment isolation work.
test('local execution drops backend secrets and caps runaway output', async () => {
  const { createExecutor } = require('../src/services/execution');
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

test('all output, including the last chunk, arrives before close and multi-byte characters survive', async () => {
  const { createExecutor } = require('../src/services/execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-close-test-'));
  const script = path.join(dir, 'emit.js');
  // A progress bar redrawn in place with multi-byte characters, then a large final burst (big
  // enough to be split mid-character across pipe reads) and an immediate exit.
  fs.writeFileSync(script,
    "for (let i = 0; i < 200; i++) process.stdout.write('\\r[' + '█'.repeat(i % 40) + ']');\n" +
    "process.stdout.write('\\n' + '█'.repeat(50000) + '\\nDONE ✓\\n');\n");
  let expected = '';
  for (let i = 0; i < 200; i++) expected += '\r[' + '█'.repeat(i % 40) + ']';
  expected += '\n' + '█'.repeat(50000) + '\nDONE ✓\n';
  const executor = createExecutor({ mode: 'local', nodeEnv: 'test', binary: process.execPath });
  try {
    const child = executor.spawnCompiler([script], { cwd: dir, timeoutMs: 10000 });
    let output = '';
    child.stdout.on('data', chunk => { output += chunk; });
    const [code] = await once(child, 'close');
    assert.equal(code, 0);
    assert.ok(!output.includes('�'), 'a multi-byte character was split into replacement characters');
    assert.equal(output, expected);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('runner stops a hung program and releases the execution slot', async () => {
  const { createExecutor } = require('../src/services/execution');
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
  const { createExecutor } = require('../src/services/execution');
  assert.throws(() => createExecutor({ mode: 'local', nodeEnv: 'production' }), /production/i);
  const executor = createExecutor({ mode: 'docker', nodeEnv: 'production' });
  assert.throws(() => executor.spawnCompiler(['/etc/passwd'], { cwd: os.tmpdir() }), /job directory/i);
  assert.throws(() => executor.spawnCompiler(['--emit-bytecode', '-o', '/etc/evil'], { cwd: os.tmpdir() }), /job directory/i);
});

test('docker jobs have resource limits and no credentials, host network, or root privileges', () => {
  const { createExecutor } = require('../src/services/execution');
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
  const { createExecutor } = require('../src/services/execution');
  const prior = process.env.DOCKER_HOST;
  process.env.DOCKER_HOST = 'tcp://worker.example:2376';
  try {
    const spec = createExecutor({ mode: 'docker' }).buildCommand([], { cwd: os.tmpdir(), timeoutMs: 1200, userId: 'alice' });
    assert.ok(spec.args.includes('fsize=16777216:16777216'));
    assert.ok(spec.args.includes('/usr/bin/timeout'));
    assert.ok(spec.args.includes('1.2s'));
    assert.ok(spec.args.includes('--label'));
    // The in-container program is the worker image's compiler binary.
    assert.equal(spec.args[spec.args.indexOf('1.2s') + 1], '/opt/nova/nova');
    assert.equal(spec.env.DOCKER_HOST, process.env.DOCKER_HOST);
  } finally { prior === undefined ? delete process.env.DOCKER_HOST : process.env.DOCKER_HOST = prior; }
});

test('per-user batch quotas and the separate REPL pool preserve execution for other users', async () => {
  const { createExecutor } = require('../src/services/execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-quota-'));
  // Node runs the hung fixture directly, so this also works where shebangs do not (Windows).
  const fixture = path.join(dir, 'hang.js');
  fs.writeFileSync(fixture, 'setInterval(() => {}, 1000);\n');
  const executor = createExecutor({ mode: 'local', nodeEnv: 'test', binary: process.execPath });
  const children = [];
  const start = (userId, kind = 'batch') => {
    const child = executor.spawnCompiler([fixture], { cwd: dir, userId, kind, timeoutMs: 5000 });
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


test('startup sweep targets only this worker labels, spares running jobs, and passes Docker client env through cleanup', { skip: STAND_IN_DOCKER_SKIP }, async () => {
  const { sweepContainers } = require('../src/services/execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-docker-client-'));
  const log = path.join(dir, 'calls.jsonl');
  const previous = { PATH: process.env.PATH, DOCKER_HOST: process.env.DOCKER_HOST, DOCKER_CONFIG: process.env.DOCKER_CONFIG, NOVA_WORKER_ID: process.env.NOVA_WORKER_ID, NOVA_EXECUTION_MODE: process.env.NOVA_EXECUTION_MODE };
  const exited = 'abcdef123456', running = '123456abcdef';
  fs.writeFileSync(path.join(dir, 'docker'), `#!${process.execPath}
const fs = require('fs');
fs.appendFileSync(${JSON.stringify(log)}, JSON.stringify({ args: process.argv.slice(2), host: process.env.DOCKER_HOST, config: process.env.DOCKER_CONFIG, secret: process.env.JWT_SECRET }) + '\\n');
if (process.argv[2] === 'ps') console.log('${exited}\\n${running}');
if (process.argv[2] === 'inspect') console.log('${exited} exited ' + new Date().toISOString() + '\\n${running} running ' + new Date().toISOString());
`, { mode: 0o755 });
  Object.assign(process.env, { PATH: dir + path.delimiter + previous.PATH, DOCKER_HOST: 'tcp://fixture:2376', DOCKER_CONFIG: dir, NOVA_WORKER_ID: 'review-worker', NOVA_EXECUTION_MODE: 'docker' });
  try {
    await sweepContainers();
    const calls = fs.readFileSync(log, 'utf8').trim().split('\n').map(line => JSON.parse(line));
    assert.deepEqual(calls[0].args, ['ps', '-aq', '--filter', 'label=nova.job=true', '--filter', 'label=nova.worker=review-worker']);
    assert.deepEqual(calls[1].args, ['inspect', '--format', '{{.Id}} {{.State.Status}} {{.Created}}', exited, running]);
    // Only the finished container is removed; a recent running job may belong to a live instance.
    assert.deepEqual(calls[2].args, ['rm', '-f', exited]);
    assert.equal(calls.length, 3);
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

test('sweep selection removes only finished or over-age containers', () => {
  const { staleContainers, MAX_CONTAINER_AGE_MS } = require('../src/services/execution');
  const now = Date.parse('2026-10-21T12:00:00Z');
  const at = ms => new Date(now - ms).toISOString();
  const lines = [
    `${'a'.repeat(64)} exited ${at(1000)}`,
    `${'b'.repeat(64)} dead ${at(1000)}`,
    `${'c'.repeat(64)} running ${at(60000)}`,                    // another instance's live job
    `${'d'.repeat(64)} created ${at(1000)}`,                     // being started right now
    `${'e'.repeat(64)} running ${at(MAX_CONTAINER_AGE_MS + 1)}`, // older than any deadline
    `${'f'.repeat(64)} running not-a-date`,
  ].join('\n');
  assert.deepEqual(staleContainers(lines, now), ['a'.repeat(64), 'b'.repeat(64), 'e'.repeat(64), 'f'.repeat(64)]);
  assert.throws(() => staleContainers('../evil exited ' + at(0), now), /Invalid container ID/);
});

test('a container SIGKILL is reported as a timeout only once the deadline has passed', () => {
  const { classifyKill } = require('../src/services/execution');
  assert.equal(classifyKill(30000, 30000), 'timeout');
  assert.equal(classifyKill(30250, 30000), 'timeout');
  assert.equal(classifyKill(1200, 30000), 'memory');
  assert.equal(classifyKill(29999, 30000), 'memory');
});

test('Docker exit 137 before the deadline reports out of memory, after it a timeout', { skip: STAND_IN_DOCKER_SKIP }, async () => {
  const { createExecutor } = require('../src/services/execution');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-oom-test-'));
  const previous = process.env.PATH;
  // A stand-in docker CLI: exits 137 after the delay given as its last argument.
  // A .cmd shim is not spawnable without a shell, so this runs where shebangs work.
  fs.writeFileSync(path.join(dir, 'docker'), `#!${process.execPath}\nconst ms = Number(process.argv[process.argv.length - 1]); setTimeout(() => process.exit(137), ms);\n`, { mode: 0o755 });
  process.env.PATH = dir + path.delimiter + previous;
  try {
    const executor = createExecutor({ mode: 'docker', nodeEnv: 'test' });
    const run = async (delay, timeoutMs) => {
      const child = executor.spawnCompiler([String(delay)], { cwd: dir, timeoutMs });
      let stderr = '';
      child.stdout.resume(); child.stderr.on('data', d => { stderr += d; });
      const [code] = await once(child, 'close');
      return { code, reason: child.stopReason, stderr };
    };
    const oom = await run(10, 5000);
    assert.equal(oom.reason, 'memory');
    assert.match(oom.stderr, /Out of memory/);
    assert.equal(oom.code, 1);
    // The Node timer normally wins; either way the job is reported as timed out.
    const late = await run(400, 300);
    assert.equal(late.reason, 'timeout');
    assert.match(late.stderr, /timed out/);
  } finally { process.env.PATH = previous; fs.rmSync(dir, { recursive: true, force: true }); }
});
