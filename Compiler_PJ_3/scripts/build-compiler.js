#!/usr/bin/env node
'use strict';
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const compiler = process.env.CXX || (process.platform === 'darwin' ? 'clang++' : 'g++');
// CXXFLAGS replaces the default -O2 (e.g. sanitizer or warning builds in CI);
// NOVA_COMPILER_OUT lets several variants be built side by side.
const flags = process.env.CXXFLAGS ? process.env.CXXFLAGS.split(/\s+/).filter(Boolean) : ['-O2'];
const out = process.env.NOVA_COMPILER_OUT || (process.platform === 'win32' ? 'compiler.exe' : 'compiler');
const args = ['-std=c++17', ...flags, '-o', out,
  'compiler.cc', 'lexer.cc', 'inputbuf.cc', 'parser2.cc', 'bytecode.cc', 'autograd.cc'];
const ssh = spawnSync('pkg-config', ['--cflags', '--libs', 'libssh2'], { encoding: 'utf8' });
if (ssh.status === 0) args.push(...ssh.stdout.trim().split(/\s+/).filter(Boolean));
const result = spawnSync(compiler, args, { cwd: root, stdio: 'inherit' });
if (result.error) { console.error(`Could not run ${compiler}: ${result.error.message}`); process.exit(1); }
process.exit(result.status ?? 1);
