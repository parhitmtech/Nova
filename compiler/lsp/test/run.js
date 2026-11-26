#!/usr/bin/env node
// Protocol tests for nova-lsp: start the real server, talk to it as an editor does (JSON-RPC over stdio), and check
// its answers. Usage: node lsp/test/run.js [path to nova-lsp]   (make -C compiler lsp-test)
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const assert = require('assert/strict');

const COMPILER = path.resolve(__dirname, '..', '..');
const BINARY = process.argv[2] || path.join(COMPILER, 'build', process.platform === 'win32' ? 'nova-lsp.exe' : 'nova-lsp');

// Two URIs of the same file: file:///C%3A/a and file:///c:/a are one file on Windows, as VS Code treats them
const sameUri = (a, b) => {
  const norm = u => { const d = decodeURIComponent(u); return process.platform === 'win32' ? d.toLowerCase() : d; };
  return norm(a) === norm(b);
};

// A minimal LSP client: frames messages, matches responses to requests, keeps notifications
class Client {
  constructor() {
    this.proc = spawn(BINARY, ['--stdio'], { env: { ...process.env, NOVA_HOME: COMPILER } });
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.notifications = [];
    this.waiters = [];
    this.stderr = '';
    this.proc.stderr.on('data', d => { this.stderr += d; });
    this.proc.stdout.on('data', d => { this.buffer = Buffer.concat([this.buffer, d]); this.drain(); });
    this.exited = new Promise(resolve => this.proc.on('exit', code => resolve(code)));
  }
  drain() {
    while (true) {
      const header = this.buffer.indexOf('\r\n\r\n');
      if (header < 0) return;
      const match = /Content-Length: (\d+)/i.exec(this.buffer.slice(0, header).toString());
      const length = Number(match[1]);
      if (this.buffer.length < header + 4 + length) return;
      const message = JSON.parse(this.buffer.slice(header + 4, header + 4 + length).toString('utf8'));
      this.buffer = this.buffer.slice(header + 4 + length);
      if (message.id !== undefined && this.pending.has(message.id)) {
        this.pending.get(message.id)(message);
        this.pending.delete(message.id);
      } else {
        this.notifications.push(message);
        this.waiters = this.waiters.filter(w => !w());
      }
    }
  }
  write(message) {
    const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', ...message }), 'utf8');
    this.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.proc.stdin.write(body);
  }
  request(method, params) {
    const id = this.nextId++;
    return new Promise(resolve => { this.pending.set(id, resolve); this.write({ id, method, params }); });
  }
  notify(method, params) { this.write({ method, params }); }
  // The next diagnostics published for uri after this call
  diagnosticsFor(uri) {
    const start = this.notifications.length;
    return new Promise(resolve => {
      const check = () => {
        const found = this.notifications.slice(start).find(n => n.method === 'textDocument/publishDiagnostics' && sameUri(n.params.uri, uri));
        if (found) { resolve(found.params.diagnostics); return true; }
        return false;
      };
      if (!check()) this.waiters.push(check);
    });
  }
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

const DIR = path.join(COMPILER, 'lsp', 'test');
const fileUri = p => 'file://' + (process.platform === 'win32' ? '/' : '') + p.replace(/\\/g, '/');
const URI = fileUri(path.join(DIR, 'program.nova'));

const PROGRAM = [
  'class Point:',                                       // 0
  '    int x',                                          // 1
  '    int y',                                          // 2
  '',                                                   // 3
  '    def init(int x, int y):',                        // 4
  '        self.x = x',                                 // 5
  '        self.y = y',                                 // 6
  '',                                                   // 7
  '    def norm2() -> int:',                            // 8
  '        return self.x * self.x + self.y * self.y',   // 9
  '',                                                   // 10
  'def add(int a, int b) -> int:',                      // 11
  '    return a + b',                                   // 12
  '',                                                   // 13
  'ignite:',                                            // 14
  '    Point p = Point(3, 4)',                          // 15
  '    vector<int> v = vector<int>()',                  // 16
  '    v.push(add(p.x, p.y))',                          // 17
  '    string s = "héllo 🙂"',                          // 18
  '    print(p.norm2() + v[0])',                        // 19
  '    print(s)',                                       // 20
  '',
].join('\n');

let client;

test('initialize: the server says what it can do', async () => {
  const r = await client.request('initialize', { processId: null, rootUri: null, capabilities: {},
                                                  initializationOptions: { novaHome: COMPILER } });
  const caps = r.result.capabilities;
  assert.equal(caps.textDocumentSync.change, 1);
  assert.equal(caps.hoverProvider, true);
  assert.equal(caps.definitionProvider, true);
  assert.equal(caps.documentSymbolProvider, true);
  assert.deepEqual(caps.completionProvider.triggerCharacters, ['.']);
  assert.equal(r.result.serverInfo.name, 'nova-lsp');
  client.notify('initialized', {});
});

test('a correct program has no diagnostics', async () => {
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didOpen', { textDocument: { uri: URI, languageId: 'nova', version: 1, text: PROGRAM } });
  assert.deepEqual(await d, []);
});

test('errors as you type: codes, exact ranges in UTF-16, a link to the error reference', async () => {
  // line 18 has é (2 bytes, 1 UTF-16 unit) and 🙂 (4 bytes, 2 units) before the error
  const broken = PROGRAM.replace('    string s = "héllo 🙂"', '    string s = "héllo 🙂" + 1');
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI, version: 2 }, contentChanges: [{ text: broken }] });
  const diags = await d;
  assert.equal(diags.length, 1, JSON.stringify(diags));
  const e = diags[0];
  assert.equal(e.code, 'E0507');
  assert.equal(e.severity, 1);
  assert.equal(e.source, 'nova');
  assert.match(e.message, /cannot concatenate string with int/);
  // "    string s = " is 15 units; the string literal is 10 units ("héllo 🙂" with quotes), then " + 1"
  assert.deepEqual(e.range.start, { line: 18, character: 15 });
  assert.deepEqual(e.range.end, { line: 18, character: 29 });
  assert.match(e.codeDescription.href, /ERRORS\.md#e0507-operator-on-the-wrong-types$/);
});

test('syntax errors: every one, in one run, with a fix for an editor', async () => {
  const broken = PROGRAM.replace('    print(s)', '    print(!s);');
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI, version: 3 }, contentChanges: [{ text: broken }] });
  const diags = await d;
  assert.deepEqual(diags.map(x => x.code), ['E0112']);  // the ';' is on the same line: one error per line
  assert.deepEqual(diags[0].data.fixes[0].newText, 'not ');
});

test('warnings are faded as unnecessary', async () => {
  const withUnused = PROGRAM.replace('    print(s)', '    int unused = 3\n    print(s)');
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI, version: 4 }, contentChanges: [{ text: withUnused }] });
  const diags = await d;
  assert.equal(diags.length, 1);
  assert.equal(diags[0].code, 'W0001');
  assert.equal(diags[0].severity, 2);
  assert.deepEqual(diags[0].tags, [1]);
  // back to the correct program
  const ok = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI, version: 5 }, contentChanges: [{ text: PROGRAM }] });
  assert.deepEqual(await ok, []);
});

const hoverAt = async (line, character, expectNone = false) => {
  const r = (await client.request('textDocument/hover', { textDocument: { uri: URI }, position: { line, character } })).result;
  if (!expectNone && !r) throw new Error(`no hover at ${line}:${character}`);
  return r;
};

test('hover: a variable, a function, a field, a method, a class', async () => {
  assert.match((await hoverAt(15, 10)).contents.value, /```nova\nPoint p\n```\n\nvariable/);
  assert.match((await hoverAt(17, 12)).contents.value, /def add\(int a, int b\) -> int/);
  const field = (await hoverAt(17, 17)).contents.value;
  assert.match(field, /int x/);
  assert.match(field, /field of Point/);
  assert.match((await hoverAt(19, 13)).contents.value, /def norm2\(\) -> int[\s\S]*method of Point/);
  assert.match((await hoverAt(15, 16)).contents.value, /class Point:\n    int x\n    int y/);
  assert.equal(await hoverAt(3, 0, true), null);  // an empty line
});

const definitionAt = async (line, character) => {
  const r = (await client.request('textDocument/definition', { textDocument: { uri: URI }, position: { line, character } })).result;
  if (!r) throw new Error(`no definition at ${line}:${character}`);
  return r;
};

test('go to definition: a function, a field, a method, a variable', async () => {
  assert.deepEqual((await definitionAt(17, 12)).range.start, { line: 11, character: 4 });
  assert.deepEqual((await definitionAt(17, 17)).range.start, { line: 1, character: 8 });
  assert.deepEqual((await definitionAt(19, 13)).range.start, { line: 8, character: 8 });
  const v = await definitionAt(19, 22);
  assert.equal(v.uri, URI);
  assert.deepEqual(v.range.start, { line: 16, character: 16 });
});

test('outline: classes with their fields and methods, functions, the entry point', async () => {
  const r = (await client.request('textDocument/documentSymbol', { textDocument: { uri: URI } })).result;
  assert.deepEqual(r.map(s => [s.name, s.kind]), [['Point', 5], ['add', 12], ['ignite', 12]]);
  assert.deepEqual(r[0].children.map(c => [c.name, c.kind]), [['x', 8], ['y', 8], ['init', 6], ['norm2', 6]]);
  assert.equal(r[1].detail, '(int a, int b) -> int');
  assert.deepEqual(r[0].range.start, { line: 0, character: 0 });
});

const completeAt = async (text, line, character) => {
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI }, contentChanges: [{ text }] });
  await d;
  return (await client.request('textDocument/completion', { textDocument: { uri: URI }, position: { line, character } })).result;
};

test('completion after "." while the line does not parse yet: the fields and methods', async () => {
  const typing = PROGRAM.replace('    print(s)', '    print(s)\n    p.');
  const items = await completeAt(typing, 21, 6);
  assert.deepEqual(items.map(i => i.label).sort(), ['norm2', 'x', 'y']);  // not init
  const vec = await completeAt(PROGRAM.replace('    print(s)', '    print(s)\n    v.'), 21, 6);
  assert.ok(vec.some(i => i.label === 'push') && vec.some(i => i.label === 'size'));
  const self = await completeAt(PROGRAM.replace('        return self.x * self.x + self.y * self.y',
                                                '        return self.'), 9, 20);
  assert.deepEqual(self.map(i => i.label).sort(), ['norm2', 'x', 'y']);
});

test('completion of names: the variables in scope, the definitions, keywords and built-ins', async () => {
  const items = await completeAt(PROGRAM.replace('    print(s)', '    print(s)\n    pr'), 21, 6);
  const labels = items.map(i => i.label);
  for (const want of ['p', 'v', 's', 'add', 'Point', 'print', 'while', 'int', 'zeros']) assert.ok(labels.includes(want), want);
  assert.ok(!labels.includes('a'), "add's parameter is not in scope in ignite");
  assert.equal(items.find(i => i.label === 'p').detail, 'Point');
});

test('signature help: the open call and the argument being written', async () => {
  const text = PROGRAM.replace('    print(s)', '    print(add(1, ');
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI }, contentChanges: [{ text }] });
  await d;
  const r = (await client.request('textDocument/signatureHelp', { textDocument: { uri: URI }, position: { line: 20, character: 17 } })).result;
  assert.equal(r.signatures[0].label, 'def add(int a, int b) -> int');
  assert.deepEqual(r.signatures[0].parameters.map(p => p.label), ['int a', 'int b']);
  assert.equal(r.activeParameter, 1);
  const ctor = PROGRAM.replace('    print(s)', '    Point q = Point(');
  const d2 = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI }, contentChanges: [{ text: ctor }] });
  await d2;
  const c = (await client.request('textDocument/signatureHelp', { textDocument: { uri: URI }, position: { line: 20, character: 20 } })).result;
  assert.equal(c.signatures[0].label, 'Point(int x, int y)');
});

test('an unknown request is answered with an error, and the server goes on', async () => {
  const r = await client.request('textDocument/notAThing', {});
  assert.equal(r.error.code, -32601);
  const h = await hoverAt(15, 10, true);
  assert.ok(h === null || h.contents);
});

test('closing a document clears its diagnostics', async () => {
  const broken = PROGRAM.replace('    print(s)', '    print(nope)');
  const d = client.diagnosticsFor(URI);
  client.notify('textDocument/didChange', { textDocument: { uri: URI }, contentChanges: [{ text: broken }] });
  assert.equal((await d)[0].code, 'E0406');
  const cleared = client.diagnosticsFor(URI);
  client.notify('textDocument/didClose', { textDocument: { uri: URI } });
  assert.deepEqual(await cleared, []);
});

const MODULE_URI = fileUri(path.join(DIR, 'uses_module.nova'));

test('a module brought in with orbit: hover and go to definition reach into it', async () => {
  const text = 'orbit geometry\n\nignite:\n    print(area(2, 3))\n';
  const d = client.diagnosticsFor(MODULE_URI);
  client.notify('textDocument/didOpen', { textDocument: { uri: MODULE_URI, languageId: 'nova', version: 1, text } });
  assert.deepEqual(await d, []);
  const def = (await client.request('textDocument/definition', { textDocument: { uri: MODULE_URI }, position: { line: 3, character: 11 } })).result;
  assert.ok(def, 'no definition');
  assert.match(decodeURIComponent(def.uri), /lsp\/test\/geometry\.nova$/);
  assert.deepEqual(def.range.start, { line: 1, character: 4 });
  const h = (await client.request('textDocument/hover', { textDocument: { uri: MODULE_URI }, position: { line: 3, character: 11 } })).result;
  assert.match(h.contents.value, /def area\(int w, int h\) -> int/);
});

test("an error inside a module is published to the module's file, and cleared when it goes", async () => {
  const brokenUri = fileUri(path.join(DIR, 'broken_module.nova'));
  const d = client.diagnosticsFor(brokenUri);
  client.notify('textDocument/didChange', { textDocument: { uri: MODULE_URI }, contentChanges: [{ text: 'orbit broken_module\n\nignite:\n    print(broken(1))\n' }] });
  const diags = await d;
  assert.equal(diags.length, 1);
  assert.equal(diags[0].code, 'E0507');
  assert.equal(diags[0].range.start.line, 2);
  const cleared = client.diagnosticsFor(brokenUri);
  client.notify('textDocument/didChange', { textDocument: { uri: MODULE_URI }, contentChanges: [{ text: 'ignite:\n    print(1)\n' }] });
  assert.deepEqual(await cleared, []);
});

test('a module that cannot be found is reported on its orbit line', async () => {
  const d = client.diagnosticsFor(MODULE_URI);
  client.notify('textDocument/didChange', { textDocument: { uri: MODULE_URI }, contentChanges: [{ text: 'orbit nowhere\nignite:\n    print(1)\n' }] });
  const diags = await d;
  assert.equal(diags[0].code, 'E0302');
  assert.equal(diags[0].range.start.line, 0);
});

test('bad input from a client does not stop the server', async () => {
  const body = Buffer.from('{ not json', 'utf8');
  const start = client.notifications.length;
  client.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  client.proc.stdin.write(body);
  // a request about a document that is not open, and one with its fields missing
  assert.equal((await client.request('textDocument/hover', { textDocument: { uri: 'file:///nowhere.nova' }, position: { line: 0, character: 0 } })).result, null);
  assert.equal((await client.request('textDocument/hover', {})).result, null);
  assert.deepEqual((await client.request('textDocument/completion', { textDocument: { uri: MODULE_URI }, position: { line: 999, character: 999 } })).result.length > 0, true);
  const parseError = client.notifications.slice(start).find(n => n.error && n.error.code === -32700);
  assert.ok(parseError, 'a parse error is answered');
});

test('shutdown, then exit: the server ends with status 0', async () => {
  const r = await client.request('shutdown', null);
  assert.equal(r.result, null);
  client.notify('exit', null);
  assert.equal(await client.exited, 0);
});

(async () => {
  client = new Client();
  let failed = 0;
  for (const t of tests) {
    try {
      await Promise.race([t.fn(), new Promise((_, reject) => setTimeout(() => reject(new Error('timed out')), 10000))]);
      console.log(`  PASS  ${t.name}`);
    } catch (e) {
      failed++;
      console.log(`  FAIL  ${t.name}\n        ${String(e.message).split('\n').join('\n        ')}`);
    }
  }
  if (client.stderr.trim()) console.log('server stderr:\n' + client.stderr);
  console.log(`\nnova-lsp: ${tests.length - failed} passed, ${failed} failed`);
  client.proc.kill();
  process.exit(failed ? 1 : 0);
})();
