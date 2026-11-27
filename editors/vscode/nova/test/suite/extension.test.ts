// The extension in a real VS Code: it starts nova-lsp, and what the language server answers reaches the editor.
import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

const fixtures = path.resolve(__dirname, '..', '..', 'fixtures');
const fixture = (name: string) => vscode.Uri.file(path.join(fixtures, name));

// Waits until check() holds, or fails after a while
async function until<T>(what: string, check: () => T | undefined | Promise<T | undefined>, ms = 20000): Promise<T> {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const value = await check();
    if (value) return value;
    await new Promise(r => setTimeout(r, 100));
  }
  throw new Error(`timed out waiting for ${what}`);
}

async function open(name: string): Promise<vscode.TextDocument> {
  const doc = await vscode.workspace.openTextDocument(fixture(name));
  await vscode.window.showTextDocument(doc);
  return doc;
}

suite('Nova extension', () => {
  suiteSetup(async () => {
    const config = vscode.workspace.getConfiguration('nova');
    // A package under test (NOVA_TEST_BUNDLED) must find its own tools: the settings are cleared (an earlier run's
    // may be in the test profile)
    if (process.env.NOVA_TEST_BUNDLED) {
      for (const key of ['server.path', 'compiler.path', 'home']) await config.update(key, undefined, vscode.ConfigurationTarget.Global);
      return;
    }
    // Point the extension at the compiler's build before it starts (it starts when a .nova file opens)
    await config.update('server.path', process.env.NOVA_TEST_SERVER, vscode.ConfigurationTarget.Global);
    await config.update('compiler.path', process.env.NOVA_TEST_COMPILER, vscode.ConfigurationTarget.Global);
    await config.update('home', process.env.NOVA_TEST_HOME, vscode.ConfigurationTarget.Global);
  });

  test('a .nova file is Nova, and opening one starts the extension and its language server', async () => {
    const doc = await open('hello.nova');
    assert.strictEqual(doc.languageId, 'nova');
    const ext = vscode.extensions.getExtension('parhitmathur.nova');
    assert.ok(ext, 'the extension is installed');
    const api = await until('the extension to activate', () => (ext.isActive ? ext.exports : undefined));
    await until('the language server to run', () => api.client()?.isRunning());
    if (process.env.NOVA_TEST_BUNDLED) {
      // the tools it runs are the ones in the package
      const exe = process.platform === 'win32' ? '.exe' : '';
      for (const tool of ['nova', 'nova-lsp']) {
        assert.ok(require('fs').existsSync(path.join(ext.extensionPath, 'bin', tool + exe)), `bin/${tool} is in the package`);
      }
      assert.ok(require('fs').existsSync(path.join(ext.extensionPath, 'stdlib')), 'stdlib/ is in the package');
    }
    const commands = await vscode.commands.getCommands(true);
    for (const c of ['nova.runFile', 'nova.checkFile', 'nova.openRepl', 'nova.restartServer']) {
      assert.ok(commands.includes(c), c);
    }
  });

  test('errors as you type: the compiler\'s codes, at the exact range', async () => {
    const doc = await open('broken.nova');
    const diags = await until('diagnostics', () => {
      const d = vscode.languages.getDiagnostics(doc.uri);
      return d.length ? d : undefined;
    });
    const unknown = diags.find(d => (d.code as { value: string }).value === 'E0406');
    assert.ok(unknown, JSON.stringify(diags));
    assert.strictEqual(unknown.source, 'nova');
    assert.strictEqual(unknown.severity, vscode.DiagnosticSeverity.Error);
    assert.strictEqual(doc.getText(unknown.range), 'totl');
    assert.match(String((unknown.code as { target: vscode.Uri }).target), /ERRORS\.md#e0406/);
  });

  test('hover shows a name\'s type, and go to definition finds its declaration', async () => {
    const doc = await open('hello.nova');
    const use = doc.getText().indexOf('square(n)');
    const hovers = await until('a hover', async () => {
      const h = await vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', doc.uri, doc.positionAt(use + 1));
      return h && h.length ? h : undefined;
    });
    const text = hovers.map(h => h.contents.map(c => (typeof c === 'string' ? c : c.value)).join('')).join('');
    assert.match(text, /def square\(int x\) -> int/);
    const defs = await vscode.commands.executeCommand<(vscode.Location | vscode.LocationLink)[]>(
      'vscode.executeDefinitionProvider', doc.uri, doc.positionAt(use + 1));
    const first = defs[0];
    const range = 'targetRange' in first ? first.targetRange : first.range;
    assert.strictEqual(range.start.line, 0);
  });

  test('completion after "." lists the fields and methods', async () => {
    const doc = await open('hello.nova');
    const editor = await vscode.window.showTextDocument(doc);
    const end = doc.lineAt(doc.lineCount - 1).range.end;
    await editor.edit(e => e.insert(end, '\n    b.'));
    const at = doc.lineAt(doc.lineCount - 1).range.end;
    const list = await until('completions', async () => {
      const l = await vscode.commands.executeCommand<vscode.CompletionList>('vscode.executeCompletionItemProvider', doc.uri, at, '.');
      const labels = l.items.map(i => (typeof i.label === 'string' ? i.label : i.label.label));
      return labels.includes('area') ? labels : undefined;
    });
    assert.ok(list.includes('width') && list.includes('area'), list.join(', '));
    await vscode.commands.executeCommand('workbench.action.files.revert');
  });

  test('a quick fix corrects what the compiler says how to fix', async () => {
    const doc = await open('semicolon.nova');
    const diag = await until('the E0108 diagnostic', () =>
      vscode.languages.getDiagnostics(doc.uri).find(d => (d.code as { value: string }).value === 'E0108'));
    const actions = await vscode.commands.executeCommand<vscode.CodeAction[]>(
      'vscode.executeCodeActionProvider', doc.uri, diag.range, vscode.CodeActionKind.QuickFix.value);
    const fix = actions.find(a => a.title === "Remove the ';'");
    assert.ok(fix && fix.edit, JSON.stringify(actions.map(a => a.title)));
    assert.ok(await vscode.workspace.applyEdit(fix.edit));
    assert.ok(!doc.getText().includes(';'));
    await vscode.commands.executeCommand('workbench.action.files.revert');
  });

  test('Run File runs the program in the Nova terminal', async () => {
    await open('hello.nova');
    await vscode.commands.executeCommand('nova.runFile');
    const terminal = await until('the Nova terminal', () => vscode.window.terminals.find(t => t.name === 'Nova'));
    assert.ok(terminal);
  });
});
