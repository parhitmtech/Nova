'use strict';
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const jwt = require('jsonwebtoken');
const WebSocket = require('ws');
const security = require('../src/services/security');
process.env.JWT_SECRET = crypto.randomBytes(48).toString('base64');
process.env.AWS_EC2_METADATA_DISABLED = 'true';
process.env.NOVA_EXECUTION_TIMEOUT_MS = '100';
process.env.NOVA_REPL_IDLE_TIMEOUT_MS = '400';
process.env.NOVA_TRUST_PROXY = 'loopback';
// Only the terminal's S3 synchronization crosses a cloud boundary in these
// tests. Keep SDK imports real and reject that request without using a network.
const { S3Client } = require('@aws-sdk/client-s3');
const originalS3Send = S3Client.prototype.send;
S3Client.prototype.send = async () => { throw new Error('Cloud calls are forbidden in security tests'); };
const { server, writeTempFile, cleanupTempFile } = require('../src/server');
const users = [crypto.randomUUID(), crypto.randomUUID()];
const workspace = path.resolve(__dirname, '..', 'workspace');
let base;
function token(userId = users[0], changes = {}) {
  return jwt.sign({ userId, email: 'security@example.test', ...changes }, process.env.JWT_SECRET,
    { expiresIn: '1h', algorithm: 'HS256', issuer: 'nova', audience: 'nova-ide' });
}
async function request(route, { auth = token(), method = 'GET', body, origin } = {}) {
  return fetch(base + route, { method, headers: { ...(auth ? { authorization: `Bearer ${auth}` } : {}), ...(body ? { 'content-type': 'application/json' } : {}), ...(origin ? { origin } : {}) }, body: body ? JSON.stringify(body) : undefined });
}
before(async () => { await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); base = `http://127.0.0.1:${server.address().port}`; });
after(async () => { S3Client.prototype.send = originalS3Send; for (const id of users) fs.rmSync(path.join(workspace, id), { recursive: true, force: true }); await new Promise(resolve => server.close(resolve)); });
test('reject missing, weak, and known JWT secrets', () => {
  for (const value of [undefined, 'novacomp-secret-key', 'a'.repeat(64), 'change-me-example-secret-1234567890']) assert.throws(() => security.validateSecret(value));
  assert.equal(security.validateSecret(process.env.JWT_SECRET), process.env.JWT_SECRET);
});
test('existing passwords remain usable while signup enforces the new policy', async () => {
  const { DynamoDBDocumentClient } = require('@aws-sdk/lib-dynamodb');
  const bcrypt = require('bcryptjs');
  const originalSend = DynamoDBDocumentClient.prototype.send;
  try {
    for (const password of ['old', 'x'.repeat(80)]) {
      const passwordHash = await bcrypt.hash(password, 4);
      DynamoDBDocumentClient.prototype.send = async () => ({ Item: { userId: users[0], passwordHash } });
      const body = { email: 'existing@example.test', password };
      assert.equal((await request('/auth/signup', { method: 'POST', body })).status, 400);
      const response = await request('/auth/login', { method: 'POST', body });
      assert.equal(response.status, 200);
      assert.equal(security.verifyToken(jwt, (await response.json()).token, process.env.JWT_SECRET).userId, users[0]);
    }
  } finally { DynamoDBDocumentClient.prototype.send = originalSend; }
});
test('authenticate protected routes before running or touching providers', async () => {
  for (const [route, method] of [['/fs/tree', 'GET'], ['/run', 'POST'], ['/run-stream', 'POST'], ['/compile-bytecode', 'POST'], ['/compare', 'POST'], ['/gpu/submit', 'POST'], ['/gpu/status/123_abc', 'GET'], ['/api/share', 'POST'], ['/run-debug/start', 'POST']]) {
    assert.equal((await request(route, { auth: null, method, body: method === 'POST' ? {} : undefined })).status, 401, route);
  }
});
test('reject signed tokens missing validated tenant claims and issuer', async () => {
  for (const auth of [token('../victim'), token(users[0], { email: 1 }), jwt.sign({ userId: users[0], email: 'security@example.test' }, process.env.JWT_SECRET, { expiresIn: '1h' })]) {
    assert.equal((await request('/auth/me', { auth })).status, 401);
  }
  assert.equal((await request('/auth/me')).status, 200);
});
test('HTTP workspaces isolate users and reject traversal and symlink reads/writes', async () => {
  const write = await request('/fs/write', { method: 'POST', body: { path: 'private.nova', content: 'tenant one secret' } });
  assert.equal(write.status, 200);
  assert.equal((await request('/fs/read?path=private.nova', { auth: token(users[1]) })).status, 404);
  const tree = await (await request('/fs/tree', { auth: token(users[1]) })).json();
  assert.deepEqual(tree.files, []);
  for (const file of ['../' + users[0] + '/private.nova', '/etc/passwd', '../', '']) {
    assert.equal((await request('/fs/read?path=' + encodeURIComponent(file), { auth: token(users[1]) })).status, 400);
  }
  const victim = path.join(workspace, users[0], 'private.nova');
  const link = path.join(workspace, users[1], 'link.nova');
  fs.symlinkSync(victim, link);
  assert.equal((await request('/fs/read?path=link.nova', { auth: token(users[1]) })).status, 400);
  assert.equal((await request('/fs/write', { auth: token(users[1]), method: 'POST', body: { path: 'link.nova', content: 'overwrite' } })).status, 400);
  assert.equal(fs.readFileSync(victim, 'utf8'), 'tenant one secret');
});
test('artifact paths reject outside paths and symlinks including dangling ones', () => {
  const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'nova-security-')));
  try {
    fs.writeFileSync(path.join(root, 'artifact'), 'model');
    fs.symlinkSync('/etc/passwd', path.join(root, 'outside'));
    fs.symlinkSync(path.join(root, 'missing'), path.join(root, 'dangling'));
    assert.equal(security.safePath(root, '/etc/passwd', { regularFile: true }), null);
    assert.equal(security.safePath(root, '../other'), null);
    assert.equal(security.safePath(root, 'outside', { regularFile: true }), null);
    assert.equal(security.safePath(root, 'dangling'), null);
    assert.equal(security.safePath(root, 'artifact', { regularFile: true }), path.join(root, 'artifact'));
    assert.equal(security.safePath(root, 'artifact/child'), null);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});
test('reject cross-origin requests and large JSON bodies', async () => {
  assert.equal((await request('/auth/me', { origin: 'https://attacker.example' })).status, 403);
  assert.equal((await request('/fs/write', { method: 'POST', body: { path: 'large', content: 'a'.repeat(70000) } })).status, 413);
});
test('backend source and temporary files are never served as static assets', async () => {
  for (const route of ['/server.js', '/security.js', '/package.json', '/workspace/' + users[0] + '/private.nova']) {
    const res = await request(route, { auth: null });
    const body = await res.text();
    assert.ok(!body.includes('tenant one secret') && !body.includes('validateSecret(process.env.JWT_SECRET)') && !body.includes('"jsonwebtoken"'), route);
  }
});
test('debug and production GPU capabilities fail explicitly', async () => {
  assert.equal((await request('/run-debug/start', { method: 'POST', body: {} })).status, 501);
  assert.equal((await request('/gpu/submit', { method: 'POST', body: {} })).status, 503);
});
test('invalid websocket tokens and origins reject the upgrade', async () => {
  for (const [route, auth, origin, expected] of [['/terminal', 'bad', base, 401], ['/repl', 'bad', base, 401], ['/terminal', token(), 'https://attacker.example', 403]]) {
    await new Promise((resolve, reject) => {
      const ws = new WebSocket(base.replace('http:', 'ws:') + route + '?token=' + auth, { origin });
      ws.on('unexpected-response', (_req, res) => { try { assert.equal(res.statusCode, expected); res.resume(); resolve(); } catch (e) { reject(e); } });
      ws.on('open', () => { ws.close(); reject(new Error('Unauthenticated socket opened')); });
      ws.on('error', reject);
    });
  }
});
test('finished execution jobs are removed without deleting neighboring files', () => {
  const job = writeTempFile('print(1);');
  assert.ok(fs.existsSync(job.tmpFile));
  cleanupTempFile(job.tmpFile, job.tmpDir);
  assert.equal(fs.existsSync(job.tmpDir), false);
  cleanupTempFile(null, workspace);
  assert.ok(fs.existsSync(workspace));
});

test('GPU metadata rejects jobs belonging to another user or missing ownership', async () => {
  const meta = path.resolve(__dirname, '..', '_tmp', 'gpu_job_123_abc.json');
  const previous = { enabled: process.env.NOVA_ENABLE_GPU, mode: process.env.NOVA_EXECUTION_MODE, nodeEnv: process.env.NODE_ENV };
  process.env.NOVA_ENABLE_GPU = 'true'; process.env.NOVA_EXECUTION_MODE = 'local'; process.env.NODE_ENV = 'test';
  try {
    assert.deepEqual(await (await request('/api/capabilities')).json(), { gpu: true, ml: false });
    fs.writeFileSync(meta, JSON.stringify({ ownerId: users[0], status: 'completed', type: 'local', output: 'secret GPU output' }));
    for (const route of ['/gpu/status/123_abc', '/gpu/results/123_abc']) {
      assert.equal((await request(route, { auth: token(users[1]) })).status, 404);
      assert.equal((await request(route)).status, 200);
    }
    fs.writeFileSync(meta, JSON.stringify({ status: 'completed', type: 'local' }));
    assert.equal((await request('/gpu/results/123_abc')).status, 404);
  } finally {
    fs.rmSync(meta, { force: true });
    for (const [name, value] of [['NOVA_ENABLE_GPU', previous.enabled], ['NOVA_EXECUTION_MODE', previous.mode], ['NODE_ENV', previous.nodeEnv]]) value === undefined ? delete process.env[name] : process.env[name] = value;
  }
});
test('terminal home and output options stay inside the authenticated workspace', async () => {
  await request('/fs/write', { method: 'POST', body: { path: 'program.nova', content: 'print(1);' } });
  const sibling = path.join(workspace, users[1], 'program.nova');
  fs.writeFileSync(sibling, 'tenant two secret');
  await new Promise((resolve, reject) => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/terminal', { origin: base });
    ws.on('open', () => ws.send(JSON.stringify({ type: 'auth', token: token() })));
    let text = ''; let sent = false;
    const timer = setTimeout(() => { ws.close(); reject(new Error('Terminal regression timed out')); }, 3000);
    ws.on('message', data => {
      text += data.toString();
      if (!sent && text.includes('novacomp')) {
        sent = true; text = '';
        ws.send(JSON.stringify({ type: 'input', data: 'cd ~\rpwd\rnovacomp program.nova -o ../escaped-output\rcat ../' + users[1] + '/program.nova\r' }));
      } else if (sent && text.includes('cat: permission denied')) {
        clearTimeout(timer);
        try {
          assert.ok(text.includes('invalid output path'));
          assert.ok(!text.includes('tenant two secret'));
          assert.equal(fs.existsSync(path.join(workspace, 'escaped-output')), false);
          ws.close(); resolve();
        } catch (e) { ws.close(); reject(e); }
      }
    });
    ws.on('error', e => { clearTimeout(timer); reject(e); });
  });
});
test('stream worker startup failures leave no temporary files or execution slots', async () => {
  const tmpRoot = path.resolve(__dirname, '..', '_tmp');
  const before = fs.readdirSync(tmpRoot).sort();
  const prior = process.env.NOVA_EXECUTION_MODE;
  process.env.NOVA_EXECUTION_MODE = 'invalid-mode-for-security-test';
  try {
    for (let i = 0; i < 6; i++) {
      const res = await request('/run-stream', { method: 'POST', body: { source: 'print(1);' } });
      assert.equal(res.status, 503);
      const body = await res.json();
      assert.match(body.error, /Unknown Nova execution mode/);
    }
    assert.deepEqual(fs.readdirSync(tmpRoot).sort(), before);
  } finally { prior === undefined ? delete process.env.NOVA_EXECUTION_MODE : process.env.NOVA_EXECUTION_MODE = prior; }
});
test('auth request floods are rate limited before provider access', async () => {
  let last;
  for (let i = 0; i < 11; i++) last = await request('/auth/login', { auth: null, method: 'POST', body: {} });
  assert.equal(last.status, 429);
});

test('real local compiler streaming timeout and client cancellation clean up jobs', async () => {
  const prior = process.env.NOVA_EXECUTION_MODE;
  const priorEnv = process.env.NODE_ENV;
  process.env.NOVA_EXECUTION_MODE = 'local'; process.env.NODE_ENV = 'test';
  const tmpRoot = path.resolve(__dirname, '..', '_tmp');
  const before = fs.readdirSync(tmpRoot).sort();
  const source = 'ignite:\n    int x = 0\n    while (x < 2000000000): x = x + 1\n';
  try {
    const response = await request('/run-stream', { method: 'POST', body: { source } });
    assert.equal(response.status, 200);
    const output = await response.text();
    assert.match(output, /Execution timed out/);
    assert.match(output, /"type":"done"/);
    assert.deepEqual(fs.readdirSync(tmpRoot).sort(), before);
    const controller = new AbortController();
    const stream = await fetch(base + '/run-stream', { method: 'POST', signal: controller.signal, headers: { authorization: 'Bearer ' + token(), 'content-type': 'application/json' }, body: JSON.stringify({ source }) });
    assert.equal(stream.status, 200);
    controller.abort();
    for (let i = 0; i < 50 && JSON.stringify(fs.readdirSync(tmpRoot).sort()) !== JSON.stringify(before); i++) await new Promise(resolve => setTimeout(resolve, 20));
    assert.deepEqual(fs.readdirSync(tmpRoot).sort(), before);
  } finally {
    prior === undefined ? delete process.env.NOVA_EXECUTION_MODE : process.env.NOVA_EXECUTION_MODE = prior;
    priorEnv === undefined ? delete process.env.NODE_ENV : process.env.NODE_ENV = priorEnv;
  }
});

test('public health reveals only status and capabilities explicitly disable GPU/ML', async () => {
  assert.deepEqual(await (await request('/health', { auth: null })).json(), { status: 'ok' });
  const res = await request('/api/capabilities');
  assert.equal(res.status, 200);
  assert.deepEqual(await res.json(), { gpu: false, ml: false });
});

test('trusted workspace roots may have symlinked ancestors but descendants cannot be symlinks', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-root-link-'));
  try {
    fs.mkdirSync(path.join(dir, 'real')); fs.symlinkSync(path.join(dir, 'real'), path.join(dir, 'alias'));
    const expected = path.join(fs.realpathSync(dir), 'real', 'file');
    assert.equal(security.safePath(path.join(dir, 'alias'), 'file'), expected);
    fs.symlinkSync('/etc/passwd', path.join(dir, 'real', 'link'));
    assert.equal(security.safePath(path.join(dir, 'alias'), 'link'), null);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('configured origins are an exclusive allowlist', () => {
  const req = { socket: {}, headers: { host: 'attacker.test' } };
  assert.equal(security.originAllowed('http://attacker.test', req, 'https://nova.example'), false);
});

test('production requires explicit origins before startup', () => {
  const { spawnSync } = require('node:child_process');
  const child = spawnSync(process.execPath, ['src/server.js'], { cwd: path.resolve(__dirname, '..'), env: { ...process.env, NODE_ENV: 'production', NOVA_ALLOWED_ORIGINS: '', NOVA_EXECUTION_MODE: 'docker' }, timeout: 3000 });
  assert.notEqual(child.status, 0);
  assert.match(child.stderr.toString(), /NOVA_ALLOWED_ORIGINS/);
});

test('HTTP filesystem quotas reject writes atomically and isolate each user budget', async () => {
  const previous = process.env.NOVA_WORKSPACE_MAX_BYTES;
  process.env.NOVA_WORKSPACE_MAX_BYTES = '32';
  try {
    const body = { path: 'quota-file', content: 'q'.repeat(33) };
    assert.equal((await request('/fs/write', { method: 'POST', body })).status, 413);
    assert.equal((await request('/fs/read?path=quota-file')).status, 404);
    assert.equal((await request('/fs/write', { auth: token(users[1]), method: 'POST', body: { path: 'small', content: 'ok' } })).status, 200);
  } finally { previous === undefined ? delete process.env.NOVA_WORKSPACE_MAX_BYTES : process.env.NOVA_WORKSPACE_MAX_BYTES = previous; }
});

test('filesystem requests are limited per user before exhausting storage', async () => {
  let response;
  for (let i = 0; i < 121; i++) response = await request('/fs/tree');
  assert.equal(response.status, 429);
  assert.equal((await request('/fs/tree', { auth: token(users[1]) })).status, 200);
});

test('sockets authenticate in their first message before creating a terminal', async () => {
  for (const payload of [{ type: 'input', data: 'pwd' }, { type: 'auth', token: 'invalid' }]) {
    await new Promise((resolve, reject) => {
      const ws = new WebSocket(base.replace('http:', 'ws:') + '/terminal', { origin: base });
      ws.on('open', () => ws.send(JSON.stringify(payload)));
      ws.on('message', () => reject(new Error('Unauthenticated socket received terminal data')));
      ws.on('close', code => { try { assert.equal(code, 4001); resolve(); } catch (e) { reject(e); } });
      ws.on('error', reject);
    });
  }
});

test('reverse proxy client IPs receive independent auth budgets', async () => {
  const login = ip => fetch(base + '/auth/login', { method: 'POST', headers: { 'content-type': 'application/json', 'x-forwarded-for': ip, 'x-forwarded-proto': 'https' }, body: '{}' });
  for (let i = 0; i < 10; i++) assert.equal((await login('198.51.100.10')).status, 400);
  assert.equal((await login('198.51.100.10')).status, 429);
  assert.equal((await login('198.51.100.11')).status, 400);
  assert.equal(security.originAllowed('https://nova.test', { protocol: 'https', socket: {}, headers: { host: 'nova.test' } }, ''), true);
});

test('batch slot exhaustion returns 503 on all HTTP execution routes and releases capacity', async () => {
  const { spawnCompiler } = require('../src/services/execution');
  const job = writeTempFile('');
  const children = [0, 1].map(() => spawnCompiler(['--repl'], { cwd: job.tmpDir, userId: users[0], timeoutMs: 5000 }));
  children.forEach(child => { child.stdout.resume(); child.stderr.resume(); });
  try {
    for (const route of ['/run', '/run-stream', '/compile-bytecode', '/compare']) {
      const res = await request(route, { method: 'POST', body: { source: 'ignite:\n    print(42)\n' } });
      assert.equal(res.status, 503, route);
      assert.match((await res.json()).error, /busy/i);
    }
    assert.equal((await request('/run', { auth: token(users[1]), method: 'POST', body: { source: 'ignite:\n    print(42)\n' } })).status, 200);
  } finally {
    const closed = children.map(child => new Promise(resolve => child.once('close', resolve)));
    children.forEach(child => child.kill()); await Promise.all(closed); cleanupTempFile(null, job.tmpDir);
  }
  assert.equal((await request('/run', { method: 'POST', body: { source: 'ignite:\n    print(42)\n' } })).status, 200);
});

test('active REPL input resets the idle deadline and closes once input stops', async () => {
  await new Promise((resolve, reject) => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/repl', { origin: base });
    let interval; let sent = 0; let readyAt = 0;
    const guard = setTimeout(() => { clearInterval(interval); ws.close(); reject(new Error('REPL idle regression timed out')); }, 3000);
    ws.on('open', () => ws.send(JSON.stringify({ type: 'auth', token: token(users[1]) })));
    ws.on('message', raw => {
      if (JSON.parse(raw.toString()).type !== 'ready') return;
      readyAt = Date.now();
      interval = setInterval(() => {
        ws.send(JSON.stringify({ type: 'input', line: '' }));
        if (++sent === 5) clearInterval(interval);
      }, 120);
    });
    ws.on('close', () => {
      clearTimeout(guard); clearInterval(interval);
      try { assert.equal(sent, 5); assert.ok(Date.now() - readyAt >= 900); resolve(); } catch (e) { reject(e); }
    });
    ws.on('error', reject);
  });
});

test('real bytecode API returns artifacts larger than 16 KiB', async () => {
  // One-space indentation keeps 900 statements under the 10,000-character source limit.
  const source = 'ignite:\n' + ' print(1)\n'.repeat(900);
  const res = await request('/compile-bytecode', { method: 'POST', body: { source } });
  assert.equal(res.status, 200, await (res.status === 200 ? Promise.resolve('') : res.text()));
  assert.ok((await res.arrayBuffer()).byteLength > 16384);
});

test('malformed WebSocket upgrades cannot consume authentication capacity', async () => {
  const http = require('node:http');
  for (let i = 0; i < 25; i++) {
    await new Promise((resolve, reject) => {
      const req = http.request(base + '/terminal', { headers: { origin: base, connection: 'Upgrade', upgrade: 'websocket', 'sec-websocket-key': 'invalid', 'sec-websocket-version': '13' } }, res => {
        res.resume();
        try { assert.equal(res.statusCode, 400); resolve(); } catch (e) { reject(e); }
      });
      req.on('error', reject); req.end();
    });
  }
  await new Promise((resolve, reject) => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/terminal', { origin: base });
    ws.on('open', () => ws.send(JSON.stringify({ type: 'auth', token: 'bad' })));
    ws.on('close', code => { try { assert.equal(code, 4001); resolve(); } catch (e) { reject(e); } });
    ws.on('error', reject);
  });
});
