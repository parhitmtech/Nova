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
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/terminal?token=' + token(), { origin: base });
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
