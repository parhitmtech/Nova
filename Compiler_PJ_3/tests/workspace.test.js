'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

test('workspace byte and entry quotas apply to overwrites, appends, copies and nested directories', () => {
  const { createWorkspaceStore } = require('../workspace-store');
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'nova-storage-'));
  try {
    const store = createWorkspaceStore(root, { maxBytes: 20, maxEntries: 4 });
    store.write(path.join(root, 'one'), '1234567890');
    store.write(path.join(root, 'two'), '1234567890');
    assert.throws(() => store.write(path.join(root, 'three'), 'x'), e => e.status === 413);
    assert.throws(() => store.write(path.join(root, 'one'), 'x', { append: true }), /quota/i);
    assert.throws(() => store.copy(path.join(root, 'one'), path.join(root, 'three')), /quota/i);
    store.write(path.join(root, 'one'), 'x');
    store.copy(path.join(root, 'one'), path.join(root, 'three'));
    assert.throws(() => store.mkdir(path.join(root, 'nested', 'deeper')), /quota/i);
    assert.equal(fs.existsSync(path.join(root, 'nested')), false);
    fs.unlinkSync(path.join(root, 'two'));
    store.mkdir(path.join(root, 'nested', 'deeper'));
    assert.equal(fs.readFileSync(path.join(root, 'one'), 'utf8'), 'x');
    assert.throws(() => store.write(path.join(root, '..', 'escape'), 'x'), /path/i);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});
