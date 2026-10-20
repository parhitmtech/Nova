'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { execFile } = require('node:child_process');
const execFileAsync = require('node:util').promisify(execFile);
const { createExecutor } = require('../execution');

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-worker-'));
  try {
    const source = path.join(dir, 'program.nova');
    fs.writeFileSync(source, 'main() { print(42) ; }\n');
    const executor = createExecutor({ mode: 'docker', nodeEnv: 'production' });
    const child = executor.spawnCompiler([source], { cwd: dir });
    let stdout = '', stderr = '';
    child.stdout.on('data', data => { stdout += data });
    child.stderr.on('data', data => { stderr += data });
    const [code] = await once(child, 'close');
    assert.equal(code, 0, stderr);
    assert.equal(stdout.trim(), '42');
    console.log('Production worker compiled and ran a real Nova program.');
    fs.writeFileSync(source, 'main() { ' + 'print(1) ; '.repeat(900) + '}');
    const output = path.join(dir, 'large.nbc');
    const compilation = executor.spawnCompiler([source, '--emit-bytecode', '-o', output], { cwd: dir, userId: 'worker-test' });
    let errors = '';
    compilation.stdout.resume(); compilation.stderr.on('data', data => { errors += data; });
    const [compiled] = await once(compilation, 'close');
    assert.equal(compiled, 0, errors);
    assert.ok(fs.statSync(output).size > 16384, 'worker must write bytecode larger than 16 KiB');
    console.log('Production worker wrote an artifact larger than 16 KiB.');

    // Launch directly in detached mode: no executor/Node timer can stop this job.
    fs.writeFileSync(source, 'main() { int x = 0 ; while x < 2000000000 { x = x + 1 ; } }');
    const spec = executor.buildCommand([source], { cwd: dir, timeoutMs: 2000 });
    spec.args.splice(spec.args.indexOf('--rm'), 1); // retain state for timeout verification
    spec.args.splice(1, 0, '--detach');
    await execFileAsync(spec.command, spec.args, { env: spec.env, timeout: 30000 });
    try {
      const { stdout: running } = await execFileAsync('docker', ['inspect', '--format', '{{.State.Running}}', spec.containerName], { env: spec.env, timeout: 5000 });
      assert.equal(running.trim(), 'true', 'deadline fixture must start and remain running');
      const { stdout: exitCode } = await execFileAsync('docker', ['wait', spec.containerName], { env: spec.env, timeout: 10000 });
      assert.equal(exitCode.trim(), '137', 'container must receive SIGKILL from its internal deadline');
      console.log('Detached worker hit its internal deadline without a Node execution timer.');
    } finally {
      await execFileAsync('docker', ['rm', '-f', spec.containerName], { env: spec.env, timeout: 5000 }).catch(() => {});
    }
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
})().catch(error => { console.error(error.message); process.exitCode = 1; });
