#!/usr/bin/env node
// Puts Nova's tools into the extension before a platform's package is made (vsce package --target <target>):
// bin/nova and bin/nova-lsp from the compiler's build, and stdlib/ (the standard library 'orbit' reads). The
// extension finds them there first (src/extension.ts), so the package works with nothing else installed.
//   node scripts/bundle.js <target> [build directory]     target: linux-x64, win32-x64, ...
'use strict';
const fs = require('fs');
const path = require('path');

const target = process.argv[2];
if (!target) {
  console.error('usage: node scripts/bundle.js <target> [build directory]');
  process.exit(1);
}
const root = path.resolve(__dirname, '..');
const compiler = path.resolve(root, '..', '..', '..', 'compiler');
const build = path.resolve(process.argv[3] || path.join(compiler, 'build'));
const windows = target.startsWith('win32');

fs.rmSync(path.join(root, 'bin'), { recursive: true, force: true });
fs.rmSync(path.join(root, 'stdlib'), { recursive: true, force: true });
fs.mkdirSync(path.join(root, 'bin'));
for (const tool of ['nova', 'nova-lsp']) {
  const name = windows ? `${tool}.exe` : tool;
  const from = path.join(build, name);
  if (!fs.existsSync(from)) {
    console.error(`missing ${from}: build it first (make -C compiler, make -C compiler lsp)`);
    process.exit(1);
  }
  fs.copyFileSync(from, path.join(root, 'bin', name));
  if (!windows) fs.chmodSync(path.join(root, 'bin', name), 0o755);
}
fs.cpSync(path.join(compiler, 'stdlib'), path.join(root, 'stdlib'), { recursive: true });
console.log(`bundled nova, nova-lsp and stdlib/ for ${target} from ${build}`);
