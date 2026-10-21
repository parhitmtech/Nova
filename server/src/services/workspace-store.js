'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { safePath } = require('./security');

const DEFAULT_MAX_BYTES = 50 * 1024 * 1024;
const DEFAULT_MAX_ENTRIES = 1000;

// Bytes and entries (files and directories) beneath dir, or of a single file.
function measure(target) {
  const stat = fs.lstatSync(target);
  if (!stat.isDirectory()) return { bytes: stat.isFile() ? stat.size : 0, entries: 1 };
  let bytes = 0, entries = 0;
  (function visit(dir) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      entries++;
      const file = path.join(dir, entry.name);
      if (entry.isDirectory()) visit(file);
      else if (entry.isFile()) bytes += fs.lstatSync(file).size;
    }
  })(target);
  return { bytes, entries: entries + 1 };
}

// All persistent local workspace mutations are synchronous: quota checks and
// writes cannot interleave with another request in this server process.
// Usage totals are measured once, then kept up to date by each mutation, so a
// write costs no recursive walk. A mutation that would exceed the quota (or
// fails midway) re-measures first, so drift can never cause a false rejection.
function createWorkspaceStore(root, options = {}) {
  const lexicalRoot = path.resolve(root);
  root = fs.realpathSync(root);
  // Read per check, so operators (and tests) can adjust limits without a restart.
  function limits() {
    const maxBytes = options.maxBytes ?? Number(process.env.NOVA_WORKSPACE_MAX_BYTES || DEFAULT_MAX_BYTES);
    const maxEntries = options.maxEntries ?? Number(process.env.NOVA_WORKSPACE_MAX_ENTRIES || DEFAULT_MAX_ENTRIES);
    if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 || !Number.isSafeInteger(maxEntries) || maxEntries < 1)
      throw new Error('Invalid workspace quota configuration');
    return { maxBytes, maxEntries };
  }
  limits();
  const rootUsage = () => { const { bytes, entries } = measure(root); return { bytes, entries: entries - 1 }; };
  let usage = rootUsage();

  function resolve(target, options) {
    const resolved = safePath(lexicalRoot, target, options);
    if (!resolved) throw new Error('Invalid workspace path');
    return resolved;
  }
  // Validate a write of `size` bytes to target (a file, or a directory) and
  // return the change it makes to the totals.
  function check(target, size = 0, directory = false) {
    target = resolve(target);
    const exists = fs.existsSync(target);
    const old = exists ? fs.lstatSync(target) : null;
    if (old && (directory ? !old.isDirectory() : !old.isFile())) throw new Error('Invalid workspace entry type');
    let missing = 0;
    for (let p = directory ? target : path.dirname(target); p !== root && !fs.existsSync(p); p = path.dirname(p)) missing++;
    const delta = { bytes: size - (old?.isFile() ? old.size : 0), entries: missing + (!directory && !exists ? 1 : 0) };
    const { maxBytes, maxEntries } = limits();
    const exceeds = () => usage.bytes + delta.bytes > maxBytes || usage.entries + delta.entries > maxEntries;
    if (exceeds()) usage = rootUsage();
    if (exceeds()) throw Object.assign(new Error('Workspace storage quota exceeded'), { status: 413 });
    return { target, delta };
  }
  function apply(delta, mutate) {
    try { mutate(); }
    catch (e) { usage = rootUsage(); throw e; }
    usage.bytes += delta.bytes; usage.entries += delta.entries;
  }

  function write(target, content, { append = false } = {}) {
    if (typeof content !== 'string' && !Buffer.isBuffer(content)) throw Object.assign(new Error('Invalid file content'), { status: 400 });
    const previous = append && fs.existsSync(target) ? fs.lstatSync(target).size : 0;
    const { target: file, delta } = check(target, previous + Buffer.byteLength(content));
    apply(delta, () => {
      fs.mkdirSync(path.dirname(file), { recursive: true });
      if (append) fs.appendFileSync(file, content); else fs.writeFileSync(file, content);
    });
  }
  function mkdir(target) {
    const { target: dir, delta } = check(target, 0, true);
    apply(delta, () => fs.mkdirSync(dir, { recursive: true }));
  }
  function copy(source, target) {
    source = resolve(source, { regularFile: true });
    const { target: file, delta } = check(target, fs.lstatSync(source).size);
    apply(delta, () => { fs.mkdirSync(path.dirname(file), { recursive: true }); fs.copyFileSync(source, file); });
  }
  function rename(source, target) {
    source = resolve(source);
    // Moving an existing entry adds only missing destination parent directories.
    const parent = path.dirname(target);
    if (parent !== root) mkdir(parent);
    target = resolve(target);
    // Renaming over an existing entry frees what it held.
    const replaced = fs.existsSync(target) ? measure(target) : { bytes: 0, entries: 0 };
    apply({ bytes: -replaced.bytes, entries: -replaced.entries }, () => fs.renameSync(source, target));
  }
  function remove(target, { recursive = true } = {}) {
    target = resolve(target);
    if (!fs.existsSync(target)) return;
    const removed = recursive || !fs.lstatSync(target).isDirectory() ? measure(target) : { bytes: 0, entries: 1 };
    apply({ bytes: -removed.bytes, entries: -removed.entries }, () => fs.rmSync(target, { recursive, force: true }));
  }
  return {
    write, mkdir, copy, rename, remove,
    get maxBytes() { return limits().maxBytes; },
    usage: () => ({ ...usage }),
  };
}

// One store per workspace root, shared by HTTP routes and terminals, so the
// cached totals see every mutation made through this process.
const stores = new Map();
const MAX_CACHED_STORES = 1000;
function getWorkspaceStore(root) {
  const key = fs.realpathSync(root);
  let store = stores.get(key);
  if (store) { stores.delete(key); stores.set(key, store); return store; } // keep recently used last
  store = createWorkspaceStore(key);
  stores.set(key, store);
  if (stores.size > MAX_CACHED_STORES) stores.delete(stores.keys().next().value);
  return store;
}
module.exports = { createWorkspaceStore, getWorkspaceStore };
