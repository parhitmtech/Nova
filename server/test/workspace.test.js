'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

test('workspace byte and entry quotas apply to overwrites, appends, copies and nested directories', () => {
  const { createWorkspaceStore } = require('../src/services/workspace-store');
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

// Fresh measurement of the totals the store caches.
function measured(root) {
  let bytes = 0, entries = 0;
  (function visit(dir) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      entries++;
      if (entry.isDirectory()) visit(path.join(dir, entry.name));
      else bytes += fs.lstatSync(path.join(dir, entry.name)).size;
    }
  })(root);
  return { bytes, entries };
}

test('workspace usage totals are cached across mutations instead of re-walking the tree', () => {
  const { createWorkspaceStore } = require('../src/services/workspace-store');
  const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'nova-usage-')));
  const originalReaddir = fs.readdirSync;
  try {
    fs.mkdirSync(path.join(root, 'existing'));
    fs.writeFileSync(path.join(root, 'existing', 'a'), 'abc');
    const store = createWorkspaceStore(root, { maxBytes: 1_000_000, maxEntries: 1000 });
    assert.deepEqual(store.usage(), { bytes: 3, entries: 2 });
    let walks = 0;
    fs.readdirSync = (...args) => { walks++; return originalReaddir(...args); };
    for (let i = 0; i < 50; i++) store.write(path.join(root, 'many', `f${i}`), 'x'.repeat(i));
    store.write(path.join(root, 'many', 'f1'), 'more', { append: true });
    store.write(path.join(root, 'many', 'f2'), 'replaced');
    store.copy(path.join(root, 'existing', 'a'), path.join(root, 'copy', 'a'));
    store.mkdir(path.join(root, 'd1', 'd2'));
    store.rename(path.join(root, 'copy', 'a'), path.join(root, 'moved', 'a'));
    store.rename(path.join(root, 'many', 'f3'), path.join(root, 'many', 'f4')); // overwrites f4
    assert.equal(walks, 0, 'writes must not walk the workspace');
    store.remove(path.join(root, 'existing', 'a'));
    fs.readdirSync = originalReaddir;
    store.remove(path.join(root, 'd1'));
    assert.deepEqual(store.usage(), measured(root));
  } finally { fs.readdirSync = originalReaddir; fs.rmSync(root, { recursive: true, force: true }); }
});

test('cached totals re-measure before rejecting, so outside deletions never cause a false quota error', () => {
  const { createWorkspaceStore, getWorkspaceStore } = require('../src/services/workspace-store');
  const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'nova-drift-')));
  try {
    const store = createWorkspaceStore(root, { maxBytes: 10, maxEntries: 10 });
    store.write(path.join(root, 'big'), '1234567890');
    fs.unlinkSync(path.join(root, 'big')); // not through the store
    store.write(path.join(root, 'next'), '123456789');
    assert.deepEqual(store.usage(), { bytes: 9, entries: 1 });
    assert.throws(() => store.write(path.join(root, 'over'), 'xx'), e => e.status === 413);
    // HTTP routes and terminals share one store (and one set of totals) per workspace.
    assert.equal(getWorkspaceStore(root), getWorkspaceStore(path.join(root, '.')));
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});
