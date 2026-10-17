'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { createExecutor } = require('../src/services/execution');

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
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
})().catch(error => { console.error(error.message); process.exitCode = 1; });
