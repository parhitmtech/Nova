const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');

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
