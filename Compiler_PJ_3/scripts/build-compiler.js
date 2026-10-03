#!/usr/bin/env node
'use strict';
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const compiler = process.env.CXX || (process.platform === 'darwin' ? 'clang++' : 'g++');
const args = ['-std=c++17', '-O2', '-o', process.platform === 'win32' ? 'compiler.exe' : 'compiler',
  'compiler.cc', 'lexer.cc', 'inputbuf.cc', 'parser2.cc', 'bytecode.cc', 'autograd.cc'];
const ssh = spawnSync('pkg-config', ['--cflags', '--libs', 'libssh2'], { encoding: 'utf8' });
if (ssh.status === 0) args.push(...ssh.stdout.trim().split(/\s+/).filter(Boolean));
const result = spawnSync(compiler, args, { cwd: root, stdio: 'inherit' });
if (result.error) { console.error(`Could not run ${compiler}: ${result.error.message}`); process.exit(1); }
process.exit(result.status ?? 1);
