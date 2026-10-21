'use strict';
// WebSocket terminal: a sandboxed shell confined to the user's workspace.
const fs = require('fs');
const path = require('path');
const WebSocket = require('ws');
const security = require('../services/security');
const { S3_BUCKET, WORKSPACE, MAX_EXECUTION_MS, MAX_OUTPUT_BYTES, REPL_IDLE_TIMEOUT } = require('../config');
const { log } = require('../lib/logger');
const { s3 } = require('../services/aws');
const { filterStderr, validateSource, writeTempFile, cleanupTempFile } = require('../services/compiler-runner');
const { spawnCompiler } = require('../services/execution');
const { getWorkspaceStore } = require('../services/workspace-store');
const { GetObjectCommand, ListObjectsV2Command } = require('@aws-sdk/client-s3');

// node-pty is optional — install with: npm install node-pty
// Required for the Terminal tab in the web IDE.
let pty = null;
try { pty = require('node-pty'); } catch (_) {}

// ── Shell resolver (for terminal) ─────────────────────────────────────────────

function resolveShell() {
  if (process.env.NOVA_SHELL) {
    const args = process.env.NOVA_SHELL_ARGS ? process.env.NOVA_SHELL_ARGS.split(',') : [];
    return { shell: process.env.NOVA_SHELL, args };
  }
  if (process.platform === 'win32') {
    // Default to PowerShell — always available on Windows 10/11.
    // To use WSL: set NOVA_SHELL=wsl.exe  NOVA_SHELL_ARGS=bash,-l
    return { shell: 'powershell.exe', args: [] };
  }
  return { shell: process.env.SHELL || '/bin/bash', args: [] };
}

// ── Write rate limit ──────────────────────────────────────────────────────────
// Each socket may make a burst of WRITE_BURST workspace mutations, refilled at
// WRITE_RATE_PER_SEC, so a client cannot flood the disk or the quota checks.
const WRITE_BURST = 20;
const WRITE_RATE_PER_SEC = 5;

function createTokenBucket(capacity, ratePerSec, now = Date.now) {
  let tokens = capacity;
  let last = now();
  return {
    take() {
      const t = now();
      tokens = Math.min(capacity, tokens + ((t - last) / 1000) * ratePerSec);
      last = t;
      if (tokens < 1) return false;
      tokens -= 1;
      return true;
    },
  };
}

// ── Sandboxed Workspace Shell ─────────────────────────────────────────────────
// A custom shell that runs entirely inside the IDE's workspace/ directory.
// It cannot access anything outside that folder — no real shell is spawned.
// Supports: ls, cd, pwd, cat, mkdir, rm, cp, mv, touch, echo, novacomp, clear, help

async function createShellSession(ws, userId) {
  const userWorkspace = security.userWorkspace(WORKSPACE, userId);
  fs.mkdirSync(userWorkspace, { recursive: true });
  // Every persistent write below goes through the store so storage quotas apply.
  const store = getWorkspaceStore(userWorkspace);
  const writeBucket = createTokenBucket(WRITE_BURST, WRITE_RATE_PER_SEC);

  log('info', 'Shell session started', { userId: userId || 'anonymous' });

  let cwd        = userWorkspace;   // current directory (absolute)
  let inputBuf   = '';           // typed-but-not-submitted characters
  let history    = [];           // command history
  let histIdx    = -1;           // -1 = not navigating
  let currentProc = null;        // currently running child process (for Ctrl+C)
  let histDraft = '';          // saved draft when navigating up

  // ── helpers ───────────────────────────────────────────────────────────────

  function send(text) {
    if (ws.readyState === WebSocket.OPEN) { if (ws.bufferedAmount > MAX_OUTPUT_BYTES) ws.close(1009, 'Output limit exceeded'); else ws.send(text); }
  }

  // Send a JSON control message to the browser to sync filesystem state
  function notifyFsChange(op, paths) {
    if (ws.readyState === WebSocket.OPEN)
      ws.send(JSON.stringify({ __nc: 'fs', op, paths }));
  }
  function rel(p) { return path.relative(userWorkspace, p).replace(/\\/g, '/'); }
  // Spend one write token, or tell the user why the mutation was refused.
  function allowWrite(cmd) {
    if (writeBucket.take()) return true;
    send(`\x1b[31m${cmd}: rate limit exceeded (too many workspace writes); wait a moment and try again\x1b[0m`);
    return false;
  }

  function prompt() {
    const rel = path.relative(userWorkspace, cwd).replace(/\\/g, '/');
    return `\x1b[32mnovacomp\x1b[0m:\x1b[34m${rel ? '~/' + rel : '~'}\x1b[0m$ `;
  }

  function showPrompt() { send('\r\n' + prompt()); }

  // Replace the current input line on screen with newInput
  function replaceInput(newInput) {
    if (inputBuf.length > 0) send(`\x1b[${inputBuf.length}D\x1b[K`);
    inputBuf = newInput;
    send(inputBuf);
  }

  // Resolve a user-supplied path safely — returns null if outside workspace
  function safe(p) {
    if (!p || p === '~' || p === '/') return security.safePath(userWorkspace, '', { allowRoot: true });
    const abs = p.startsWith('/') ? path.resolve(userWorkspace, '.' + p) : path.resolve(cwd, p);
    return security.safePath(userWorkspace, abs, { allowRoot: true });
  }

  // ── commands ──────────────────────────────────────────────────────────────

  function cmdLs(args) {
    const long    = args.some(a => /^-\w*l/.test(a));
    const all     = args.some(a => /^-\w*a/.test(a));
    const targets = args.filter(a => !a.startsWith('-'));
    const dir     = safe(targets[0] || '.');
    if (!dir)               { send("\x1b[31mls: permission denied\x1b[0m"); return; }
    if (!fs.existsSync(dir)){ send(`\x1b[31mls: cannot access '${targets[0]}': No such file or directory\x1b[0m`); return; }
    try {
      const entries = fs.readdirSync(dir).filter(e => all || !e.startsWith('.'));
      if (!entries.length) return;
      if (long) {
        send(entries.map(name => {
          const st  = fs.lstatSync(path.join(dir, name));
          const dir2 = st.isDirectory();
          return `${dir2 ? 'd' : '-'}rw-r--r--  ${String(dir2 ? '-' : st.size).padStart(8)}  `
               + `${st.mtime.toISOString().slice(0,10)}  `
               + (dir2 ? `\x1b[34m${name}/\x1b[0m` : name);
        }).join('\r\n'));
      } else {
        send(entries.map(name => {
          const isDir = fs.lstatSync(path.join(dir, name)).isDirectory();
          return isDir ? `\x1b[34m${name}/\x1b[0m` : name;
        }).join('  '));
      }
    } catch (e) { send(`\x1b[31mls: ${e.message}\x1b[0m`); }
  }

  function cmdCd(args) {
    const t = args[0] || '~';
    if (t === '~' || t === '/') { cwd = userWorkspace; return; }
    const p = safe(t);
    if (!p)                      { send(`\x1b[31mcd: permission denied\x1b[0m`); return; }
    if (!fs.existsSync(p))       { send(`\x1b[31mcd: ${t}: No such file or directory\x1b[0m`); return; }
    if (!fs.statSync(p).isDirectory()) { send(`\x1b[31mcd: ${t}: Not a directory\x1b[0m`); return; }
    cwd = p;
  }

  function cmdPwd() {
    send('/' + path.relative(userWorkspace, cwd).replace(/\\/g, '/'));
  }

  function cmdCat(args) {
    if (!args[0]) { send('\x1b[31mcat: missing operand\x1b[0m'); return; }
    const p = safe(args[0]);
    if (!p)                { send('\x1b[31mcat: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(p)) { send(`\x1b[31mcat: ${args[0]}: No such file or directory\x1b[0m`); return; }
    try { send(fs.readFileSync(p, 'utf8').replace(/\n/g, '\r\n')); }
    catch (e) { send(`\x1b[31mcat: ${e.message}\x1b[0m`); }
  }

  function cmdMkdir(args) {
    const targets = args.filter(a => !a.startsWith('-'));
    if (!targets.length) { send('\x1b[31mmkdir: missing operand\x1b[0m'); return; }
    for (const t of targets) {
      const p = safe(t);
      if (!p) { send('\x1b[31mmkdir: permission denied\x1b[0m'); continue; }
      if (!allowWrite('mkdir')) return;
      try { store.mkdir(p); notifyFsChange('mkdir', [rel(p)]); }
      catch (e) { send(`\x1b[31mmkdir: ${e.message}\x1b[0m`); }
    }
  }

  function cmdRm(args) {
    const recursive = args.some(a => /^-\w*r/.test(a));
    const targets   = args.filter(a => !a.startsWith('-'));
    if (!targets.length) { send('\x1b[31mrm: missing operand\x1b[0m'); return; }
    for (const t of targets) {
      const p = safe(t);
      if (!p || p === userWorkspace) { send('rm: permission denied'); continue; }
      if (!fs.existsSync(p)) { send(`\x1b[31mrm: '${t}': No such file or directory\x1b[0m`); continue; }
      if (!allowWrite('rm')) return;
      try { store.remove(p, { recursive }); notifyFsChange('delete', [rel(p)]); }
      catch (e) { send(`\x1b[31mrm: ${e.message}\x1b[0m`); }
    }
  }

  function cmdCp(args) {
    const targets = args.filter(a => !a.startsWith('-'));
    if (targets.length < 2) { send('\x1b[31mcp: missing destination\x1b[0m'); return; }
    const src = safe(targets[0]), dst = safe(targets[1]);
    if (!src || !dst)       { send('\x1b[31mcp: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(src)){ send(`\x1b[31mcp: '${targets[0]}': No such file or directory\x1b[0m`); return; }
    if (!allowWrite('cp')) return;
    try { store.copy(src, dst); notifyFsChange('update', [rel(dst)]); }
    catch (e) { send(`\x1b[31mcp: ${e.message}\x1b[0m`); }
  }

  function cmdMv(args) {
    const targets = args.filter(a => !a.startsWith('-'));
    if (targets.length < 2) { send('\x1b[31mmv: missing destination\x1b[0m'); return; }
    const src = safe(targets[0]), dst = safe(targets[1]);
    if (!src || !dst)       { send('\x1b[31mmv: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(src)){ send(`\x1b[31mmv: '${targets[0]}': No such file or directory\x1b[0m`); return; }
    if (!allowWrite('mv')) return;
    try {
      const srcRel = rel(src);
      store.rename(src, dst);
      notifyFsChange('delete', [srcRel]);
      notifyFsChange('update', [rel(dst)]);
    }
    catch (e) { send(`\x1b[31mmv: ${e.message}\x1b[0m`); }
  }

  function cmdTouch(args) {
    if (!args[0]) { send('\x1b[31mtouch: missing operand\x1b[0m'); return; }
    for (const t of args) {
      const p = safe(t);
      if (!p) { send('\x1b[31mtouch: permission denied\x1b[0m'); continue; }
      if (!allowWrite('touch')) return;
      try {
        if (fs.existsSync(p)) { const n = new Date(); fs.utimesSync(p, n, n); }
        else                  { store.write(p, ''); }
        notifyFsChange('update', [rel(p)]);
      } catch (e) { send(`\x1b[31mtouch: ${e.message}\x1b[0m`); }
    }
  }

  function cmdEcho(args) {
    const ri = args.indexOf('>'), ai = args.indexOf('>>');
    if (ri !== -1) {
      const file = args[ri + 1];
      if (!file) { send('\x1b[31mecho: missing filename after >\x1b[0m'); return; }
      const p = safe(file);
      if (!p) { send('\x1b[31mecho: permission denied\x1b[0m'); return; }
      if (!allowWrite('echo')) return;
      try { store.write(p, args.slice(0, ri).join(' ') + '\n'); notifyFsChange('update', [rel(p)]); }
      catch (e) { send(`\x1b[31mecho: ${e.message}\x1b[0m`); }
    } else if (ai !== -1) {
      const file = args[ai + 1];
      if (!file) { send('\x1b[31mecho: missing filename after >>\x1b[0m'); return; }
      const p = safe(file);
      if (!p) { send('\x1b[31mecho: permission denied\x1b[0m'); return; }
      if (!allowWrite('echo')) return;
      try { store.write(p, args.slice(0, ai).join(' ') + '\n', { append: true }); notifyFsChange('update', [rel(p)]); }
      catch (e) { send(`\x1b[31mecho: ${e.message}\x1b[0m`); }
    } else {
      send(args.join(' '));
    }
  }

  async function cmdNovacomp(args) {
    // Parse: novacomp <file.nova> [-o <outname>] [--optimize] [--dump-ir]
    let fileArg  = null;
    let outName  = 'exe';
    const extraArgs = [];
    for (let i = 0; i < args.length; i++) {
      if (args[i] === '-o' && args[i + 1]) { outName = args[++i]; }
      else if (args[i].startsWith('--'))    { extraArgs.push(args[i]); }
      else if (!fileArg)                    { fileArg = args[i]; }
    }

    if (!fileArg) {
      send('\x1b[31musage: novacomp <file.nova> [-o <name>] [--optimize] [--dump-ir]\x1b[0m');
      return;
    }

    const p = safe(fileArg);
    if (!p)                { send('\x1b[31mnovacomp: permission denied\x1b[0m'); return; }
    if (!fs.existsSync(p)) { send(`\x1b[31mnovacomp: '${fileArg}': No such file\x1b[0m`); return; }

    // Write the compiled artifact — a JSON metadata file the runtime uses.
    // When x86 codegen lands (v9), this becomes a real binary in place.
    // When JIT lands (v10), this becomes a bytecode (.nbc) file.
    const exePath = safe(outName);
    if (!exePath || exePath === userWorkspace || extraArgs.some(flag => !['--optimize', '--dump-ir'].includes(flag))) { send('novacomp: invalid output path or option'); return; }
    const meta = {
      source:    path.relative(userWorkspace, p).replace(/\\/g, '/'),
      extraArgs,
      novacomp:  '1.0',
      compiled:  new Date().toISOString(),
    };
    if (!allowWrite('novacomp')) return;
    try {
      store.write(exePath, JSON.stringify(meta, null, 2));
    } catch (e) {
      send(`\x1b[31mnovacomp: could not write '${outName}': ${e.message}\x1b[0m`);
      return;
    }

    send(`\x1b[32mCompiled\x1b[0m  ${fileArg}  →  \x1b[33m${outName}\x1b[0m\r\n\x1b[2mRun with:  ./${outName} [input1 input2 ...]\x1b[0m`);
  }

  // Run a compiled novacomp artifact: ./exe [input1 input2 ...]
  // Each positional arg is fed as one line on stdin (for input() calls).
  async function cmdRunExe(name, inputs) {
    const exeName = name.replace(/^\.\//, '');
    const exePath = safe(exeName);
    if (!exePath || !fs.existsSync(exePath)) {
      send(`\x1b[31mbash: ./${exeName}: No such file\x1b[0m`); return;
    }

    let meta;
    try {
      meta = JSON.parse(fs.readFileSync(exePath, 'utf8'));
      if (!meta.source || !meta.novacomp) throw new Error('not a novacomp artifact');
    } catch {
      send(`\x1b[31m./${exeName}: not a novacomp executable — run 'novacomp <file.nova>' first\x1b[0m`);
      return;
    }

    const srcPath = safe(meta.source);
    if (!srcPath || !fs.existsSync(srcPath)) {
      send(`\x1b[31mSource '${meta.source}' not found. Recompile with novacomp.\x1b[0m`); return;
    }

    // The compiler binary requires a .csl extension — write source to a temp file.
    const source = fs.readFileSync(srcPath, 'utf8');
    const invalid = validateSource(source);
    if (invalid) { send(invalid); return; }
    const { tmpDir, tmpFile } = writeTempFile(source);

    return new Promise(resolve => {
      const flags = Array.isArray(meta.extraArgs) ? meta.extraArgs : [];
      if (flags.some(flag => !['--optimize', '--dump-ir'].includes(flag))) { cleanupTempFile(tmpFile, tmpDir); send('Invalid compiler options'); resolve(); return; }
      let proc;
      try { proc = spawnCompiler([tmpFile, ...flags], { cwd: tmpDir, timeoutMs: MAX_EXECUTION_MS, userId }); }
      catch (err) { cleanupTempFile(tmpFile, tmpDir); send(err.message); resolve(); return; }
      currentProc = proc;
      proc.__killed = false;

      // Feed each input arg as one stdin line (for input() calls)
      if (inputs.length > 0) {
        proc.stdin.write(inputs.join('\n') + '\n', () => proc.stdin.end());
      } else {
        proc.stdin.end();
      }

      proc.stdout.on('data', d => send(d.toString().replace(/\n/g, '\r\n')));
      proc.stderr.on('data', d => {
        const f = filterStderr(d.toString());
        if (f) send('\x1b[31m' + f.replace(/\n/g, '\r\n') + '\x1b[0m');
      });
      proc.on('error', err => { send(err.message); });
      proc.on('close', code => {
        currentProc = null;
        cleanupTempFile(tmpFile, tmpDir);
        if (!proc.__killed && code !== 0 && code !== null)
          send(`\x1b[2m[process exited with code ${code}]\x1b[0m`);
        resolve();
      });
    });
  }

  function cmdHelp() {
    send([
      '\x1b[33m── NovaComp Workspace Terminal ─────────────────────────\x1b[0m',
      '  Sandboxed to the IDE workspace — cannot access your machine.',
      '',
      '  \x1b[36mls\x1b[0m [-l] [-a]            List directory',
      '  \x1b[36mcd\x1b[0m <dir>                Change directory  (~ = root)',
      '  \x1b[36mpwd\x1b[0m                      Print working directory',
      '  \x1b[36mcat\x1b[0m <file>              Show file contents',
      '  \x1b[36mtouch\x1b[0m <file>            Create empty file',
      '  \x1b[36mmkdir\x1b[0m <dir>             Create directory',
      '  \x1b[36mrm\x1b[0m [-r] <path>           Remove file or directory',
      '  \x1b[36mcp\x1b[0m <src> <dst>          Copy file',
      '  \x1b[36mmv\x1b[0m <src> <dst>          Move / rename',
      '  \x1b[36mecho\x1b[0m <text>             Print text',
      '  \x1b[36mecho\x1b[0m <text> > <file>    Write to file',
      '  \x1b[36mnovacomp\x1b[0m <file.nova>              Compile → produces \x1b[33mexe\x1b[0m',
      '  \x1b[36mnovacomp\x1b[0m <file.nova> -o <name>    Compile → produces \x1b[33m<name>\x1b[0m',
      '  \x1b[36m./<name>\x1b[0m [input1 input2 ...]       Run compiled program',
      '  \x1b[36mclear\x1b[0m                             Clear screen',
      '  \x1b[36mhelp\x1b[0m                              Show this help',
      '\x1b[33m────────────────────────────────────────────────────────\x1b[0m',
    ].join('\r\n'));
  }

  // ── command dispatcher ────────────────────────────────────────────────────

  async function execute(line) {
    const trimmed = line.trim();
    if (!trimmed) return;
    if (!history.length || history[history.length - 1] !== trimmed)
      history.push(trimmed);
      if (history.length > 100) history.shift();
    histIdx = -1; histDraft = '';

    // Tokenise (handles "quoted args")
    const parts = trimmed.match(/(?:[^\s"']+|"[^"]*"|'[^']*')+/g) || [];
    const cmd   = parts[0];
    const args  = parts.slice(1).map(a => a.replace(/^["']|["']$/g, ''));

    switch (cmd) {
      case 'ls': case 'll': cmdLs(cmd === 'll' ? ['-l', ...args] : args); break;
      case 'cd':     cmdCd(args);   break;
      case 'pwd':    cmdPwd();      break;
      case 'cat':    cmdCat(args);  break;
      case 'touch':  cmdTouch(args);break;
      case 'mkdir':  cmdMkdir(args);break;
      case 'rm':     cmdRm(args);   break;
      case 'cp':     cmdCp(args);   break;
      case 'mv':     cmdMv(args);   break;
      case 'echo':   cmdEcho(args); break;
      case 'clear':  send('\x1b[2J\x1b[H'); break;
      case 'help':   cmdHelp();     break;
      case 'novacomp': await cmdNovacomp(args); break;
      default:
        if (cmd && cmd.startsWith('./')) {
          await cmdRunExe(cmd, args);
        } else {
          send(`\x1b[31m${cmd}: command not found\x1b[0m\r\nType \x1b[36mhelp\x1b[0m to see available commands.`);
        }
    }
  }

  // ── keystroke processor ───────────────────────────────────────────────────

  async function processInput(data) {
    let i = 0;
    while (i < data.length) {
      const char = data[i];
      const code = data.charCodeAt(i);

      if (char === '\r' || char === '\n') {
        send('\r\n');
        await execute(inputBuf);
        inputBuf = '';
        showPrompt();
      } else if (char === '\x7f' || char === '\b') {          // Backspace
        if (inputBuf.length > 0) { inputBuf = inputBuf.slice(0, -1); send('\b \b'); }
      } else if (char === '\x03') {                            // Ctrl+C
        if (currentProc) {
          currentProc.__killed = true;
          try { currentProc.kill(); } catch (_) {}
          send('\r\n\x1b[2m[killed]\x1b[0m');
          // showPrompt() will be called by the awaiting execute() once the proc closes
        } else {
          send('^C'); inputBuf = ''; showPrompt();
        }
      } else if (char === '\x0c') {                            // Ctrl+L
        send('\x1b[2J\x1b[H'); showPrompt(); if (inputBuf) send(inputBuf);
      } else if (char === '\x1b' && i + 2 < data.length && data[i + 1] === '[') {
        const arrow = data[i + 2];
        if (arrow === 'A' && history.length) {                 // Up
          if (histIdx === -1) { histDraft = inputBuf; histIdx = history.length - 1; }
          else if (histIdx > 0) histIdx--;
          replaceInput(history[histIdx]);
        } else if (arrow === 'B') {                            // Down
          if (histIdx !== -1) {
            histIdx < history.length - 1 ? histIdx++ : (histIdx = -1);
            replaceInput(histIdx === -1 ? histDraft : history[histIdx]);
          }
        }
        i += 2;
      } else if (code >= 0x20 && code !== 0x7f) {             // Printable
        if (inputBuf.length < 4096) { inputBuf += char; send(char); }
      }
      i++;
    }
  }

  // ── sync user's S3 files into their workspace, then show prompt ──────────

  if (userId) {
    send('\x1b[2mLoading your workspace…\x1b[0m');
    try {
      const { Contents = [] } = await s3.send(new ListObjectsV2Command({
        Bucket: S3_BUCKET, Prefix: `${userId}/`,
      }));
      for (const obj of Contents) {
        const fileName = path.basename(obj.Key.replace(`${userId}/`, ''));
        if (!fileName || obj.Size > store.maxBytes) continue;
        const localPath = security.safePath(userWorkspace, fileName);
        if (!localPath) continue;
        try {
          if (!fs.existsSync(localPath) ||
              new Date(obj.LastModified) > fs.statSync(localPath).mtime) {
            const { Body } = await s3.send(new GetObjectCommand({ Bucket: S3_BUCKET, Key: obj.Key }));
            // Bound the download: stop reading as soon as it exceeds the quota.
            const chunks = []; let bytes = 0;
            for await (const chunk of Body) {
              bytes += chunk.length;
              if (bytes > store.maxBytes) { Body.destroy?.(); throw new Error('Workspace storage quota exceeded'); }
              chunks.push(chunk);
            }
            store.write(localPath, Buffer.concat(chunks));
          }
        } catch (_) {}
      }
    } catch (e) {
      log('error', 'Terminal S3 sync failed', { error: e.message });
    }
    send('\r\x1b[K'); // erase the "Loading" line
  }

  send('\x1b[32mNovaComp IDE\x1b[0m\r\n');
  showPrompt();

  let inputQueue = Promise.resolve();
  let pendingInputBytes = 0;
  ws.on('message', raw => {
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }
    if (msg.type === 'input') {
      if (typeof msg.data !== 'string' || pendingInputBytes + msg.data.length > 32768) return ws.close(1009, 'Input limit exceeded');
      pendingInputBytes += msg.data.length;
      inputQueue = inputQueue.then(() => processInput(msg.data)).catch(() => {}).finally(() => { pendingInputBytes -= msg.data.length; });
    } else if (msg.type === 'write' && msg.fileName) {
      const p = safe(path.basename(String(msg.fileName)));
      if (p && allowWrite('write')) try { store.write(p, String(msg.content ?? '')); } catch (_) {}
    }
  });

  const shellIdle = setTimeout(() => ws.close(1000, 'Session expired'), REPL_IDLE_TIMEOUT);
  ws.on('close', () => { clearTimeout(shellIdle); if (currentProc) currentProc.kill(); log('info', 'Shell session closed'); });
  ws.on('error', err => log('error', 'Shell WebSocket error', { error: err.message }));
}

// ── Terminal connection handler ───────────────────────────────────────────────

function handleTerminalConnection(ws, req) {
  const userId = req._userId;
  log('info', 'Terminal connection', { ip: req.socket.remoteAddress, userId });
  createShellSession(ws, userId).catch(err => { log('error', 'Shell initialization failed', { error: err.message }); ws.close(1011, 'Workspace unavailable'); });
}

module.exports = { pty, resolveShell, handleTerminalConnection, createTokenBucket };
