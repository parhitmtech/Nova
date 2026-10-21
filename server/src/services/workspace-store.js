'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { safePath } = require('./security');

// All persistent local workspace mutations are synchronous: quota checks and
// writes cannot interleave with another request in this server process.
function createWorkspaceStore(root, options = {}) {
  const lexicalRoot = path.resolve(root);
  root = fs.realpathSync(root);
  const maxBytes = options.maxBytes ?? Number(process.env.NOVA_WORKSPACE_MAX_BYTES || 50 * 1024 * 1024);
  const maxEntries = options.maxEntries ?? Number(process.env.NOVA_WORKSPACE_MAX_ENTRIES || 1000);
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 || !Number.isSafeInteger(maxEntries) || maxEntries < 1)
    throw new Error('Invalid workspace quota configuration');
  function check(target, size = 0, directory = false) {
    target = safePath(lexicalRoot, target);
    if (!target) throw new Error('Invalid workspace path');
    let bytes = 0, entries = 0;
    function visit(dir) {
      for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        entries++;
        const file = path.join(dir, entry.name);
        if (entry.isDirectory()) visit(file);
        else if (entry.isFile()) bytes += fs.lstatSync(file).size;
      }
    }
    visit(root);
    const exists = fs.existsSync(target);
    const old = exists ? fs.lstatSync(target) : null;
    if (old && (directory ? !old.isDirectory() : !old.isFile())) throw new Error('Invalid workspace entry type');
    let missing = 0;
    for (let p = directory ? target : path.dirname(target); p !== root && !fs.existsSync(p); p = path.dirname(p)) missing++;
    const projectedEntries = entries + missing + (!directory && !exists ? 1 : 0);
    const projectedBytes = bytes - (old?.isFile() ? old.size : 0) + size;
    if (projectedBytes > maxBytes || projectedEntries > maxEntries)
      throw Object.assign(new Error('Workspace storage quota exceeded'), { status: 413 });
    return target;
  }
  function write(target, content, { append = false } = {}) {
    if (typeof content !== 'string' && !Buffer.isBuffer(content)) throw Object.assign(new Error('Invalid file content'), { status: 400 });
    const previous = append && fs.existsSync(target) ? fs.lstatSync(target).size : 0;
    target = check(target, previous + Buffer.byteLength(content));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    if (append) fs.appendFileSync(target, content); else fs.writeFileSync(target, content);
  }
  function mkdir(target) { target = check(target, 0, true); fs.mkdirSync(target, { recursive: true }); }
  function copy(source, target) {
    source = safePath(lexicalRoot, source, { regularFile: true });
    if (!source) throw new Error('Invalid workspace path');
    target = check(target, fs.lstatSync(source).size);
    fs.mkdirSync(path.dirname(target), { recursive: true }); fs.copyFileSync(source, target);
  }
  function rename(source, target) {
    source = safePath(lexicalRoot, source);
    if (!source) throw new Error('Invalid workspace path');
    // Moving an existing entry adds only missing destination parent directories.
    const parent = path.dirname(target);
    if (parent !== root) mkdir(parent);
    target = safePath(lexicalRoot, target);
    if (!target) throw new Error('Invalid workspace path');
    fs.renameSync(source, target);
  }
  return { write, mkdir, copy, rename, maxBytes };
}
module.exports = { createWorkspaceStore };
