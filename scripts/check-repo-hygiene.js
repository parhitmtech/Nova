'use strict';
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

// Inspect only tracked files: local dependencies and private development
// configuration are allowed on disk, but must never enter a commit.
const files = execFileSync('git', ['ls-files', '-z'], { encoding: 'utf8' }).split('\0').filter(Boolean);
const forbidden = [];
for (const file of files) {
  const basename = path.posix.basename(file);
  const generated = /(^|\/)(node_modules|__pycache__)(\/|$)/.test(file)
    || /^(server\/(workspace|_tmp|dist)|frontend\/dist|compiler\/build)(\/|$)/.test(file)
    || /\.(exe|o|a|so|dylib|dll|nbc|pyc|pyo|cubin|ptx)$/i.test(file)
    || /^(nova|nova\.exe)$/.test(basename) || basename === '.DS_Store';
  const privateConfig = (basename === '.env' || basename.startsWith('.env.')) && basename !== '.env.example';
  const privateKey = /\.(pem|key|p12|pfx)$/i.test(file);
  let executableBinary = false;
  // Extensionless/renamed ELF, PE, and Mach-O executables must also be caught.
  if (!generated && !privateConfig && !privateKey && fs.lstatSync(file).isFile()) {
    const fd = fs.openSync(file, 'r');
    try {
      const header = Buffer.alloc(4);
      const bytes = fs.readSync(fd, header, 0, 4, 0);
      executableBinary = bytes >= 2 && header.subarray(0, 2).toString() === 'MZ'
        || bytes === 4 && ['7f454c46', 'feedface', 'feedfacf', 'cefaedfe', 'cffaedfe', 'cafebabe', 'bebafeca'].includes(header.toString('hex'));
    } finally { fs.closeSync(fd); }
  }
  if (generated || privateConfig || privateKey || executableBinary) forbidden.push(file);
}
if (forbidden.length) {
  console.error('Tracked secrets, dependencies, or generated artifacts are forbidden:\n' + forbidden.map(file => `  ${file}`).join('\n'));
  process.exitCode = 1;
} else console.log(`Repo hygiene passed (${files.length} tracked files checked).`);
