// The Nova extension: starts nova-lsp (Nova's language server) for .nova files, and adds commands to run a file,
// check it and open a REPL with Nova's interpreter. Everything the editor knows about a program comes from the
// compiler itself, through the language server.
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { LanguageClient, LanguageClientOptions, ServerOptions, State, TransportKind } from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let output: vscode.LogOutputChannel;
let status: vscode.StatusBarItem;

const exe = (name: string) => (process.platform === 'win32' ? `${name}.exe` : name);

// A tool: the path in the settings, else the one shipped in the extension (bin/), else the one on PATH
export function findTool(context: vscode.ExtensionContext, setting: string, name: string): string | undefined {
  const configured = vscode.workspace.getConfiguration('nova').get<string>(setting, '').trim();
  if (configured) return fs.existsSync(configured) ? configured : undefined;
  const bundled = context.asAbsolutePath(path.join('bin', exe(name)));
  if (fs.existsSync(bundled)) {
    // A package unzipped without its file modes leaves the program not executable: mark it again
    if (process.platform !== 'win32') {
      try { fs.accessSync(bundled, fs.constants.X_OK); } catch { try { fs.chmodSync(bundled, 0o755); } catch { /* reported when it fails to start */ } }
    }
    return bundled;
  }
  for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
    if (!dir) continue;
    const candidate = path.join(dir, exe(name));
    if (fs.existsSync(candidate)) return candidate;
  }
  return undefined;
}

// NOVA_HOME, where 'orbit' finds the standard library: the setting, else the one shipped in the extension (its
// stdlib/ folder), else the environment's
export function novaHome(context: vscode.ExtensionContext): string | undefined {
  const configured = vscode.workspace.getConfiguration('nova').get<string>('home', '').trim();
  if (configured) return configured;
  if (fs.existsSync(context.asAbsolutePath('stdlib'))) return context.extensionPath;
  return process.env.NOVA_HOME || undefined;
}

function setStatus(text: string, tooltip: string, error = false) {
  status.text = text;
  status.tooltip = tooltip;
  status.backgroundColor = error ? new vscode.ThemeColor('statusBarItem.errorBackground') : undefined;
  status.show();
}

async function startServer(context: vscode.ExtensionContext): Promise<void> {
  const server = findTool(context, 'server.path', 'nova-lsp');
  if (!server) {
    setStatus('$(error) Nova', 'nova-lsp was not found: set nova.server.path', true);
    // Not awaited: the message stays until it is dismissed, and activation (which extensions that depend on this one,
    // such as Nova Quantum, wait for) must not wait with it
    void vscode.window.showWarningMessage(
      "Nova's language server (nova-lsp) was not found, so errors are not shown as you type. If nova.server.path " +
        'is set, check it, or clear it to use the one the extension ships.',
      'Open Settings').then(choice => {
      if (choice === 'Open Settings') vscode.commands.executeCommand('workbench.action.openSettings', 'nova.server.path');
    });
    return;
  }
  const home = novaHome(context);
  const env = { ...process.env, ...(home ? { NOVA_HOME: home } : {}) };
  const serverOptions: ServerOptions = {
    run: { command: server, args: ['--stdio'], transport: TransportKind.stdio, options: { env } },
    debug: { command: server, args: ['--stdio'], transport: TransportKind.stdio, options: { env } },
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'nova' }, { scheme: 'untitled', language: 'nova' }],
    initializationOptions: { novaHome: home ?? '' },
    outputChannel: output,
    middleware: {
      // nova.warnings.enabled: false hides the warnings (unused variables, unreachable code), not the errors
      handleDiagnostics: (uri, diagnostics, next) => {
        const showWarnings = vscode.workspace.getConfiguration('nova').get<boolean>('warnings.enabled', true);
        next(uri, showWarnings ? diagnostics : diagnostics.filter(d => d.severity === vscode.DiagnosticSeverity.Error));
      },
    },
  };
  client = new LanguageClient('nova', 'Nova Language Server', serverOptions, clientOptions);
  client.onDidChangeState(e => {
    if (e.newState === State.Running) setStatus('$(check) Nova', `Nova language server: ${server}`);
    else if (e.newState === State.Stopped) setStatus('$(warning) Nova', 'The Nova language server stopped', true);
  });
  setStatus('$(sync~spin) Nova', 'Starting the Nova language server');
  try {
    await client.start();
  } catch (e) {
    setStatus('$(error) Nova', `The Nova language server did not start: ${e}`, true);
    output.appendLine(`nova-lsp did not start: ${e}`);
  }
}

async function stopServer() {
  if (!client) return;
  const c = client;
  client = undefined;
  try { await c.stop(); } catch { /* it may have stopped already */ }
}

// The terminal programs run in: one, reused, so its output stays together
function novaTerminal(home: string | undefined): vscode.Terminal {
  const existing = vscode.window.terminals.find(t => t.name === 'Nova');
  if (existing) return existing;
  return vscode.window.createTerminal({ name: 'Nova', env: home ? { NOVA_HOME: home } : {} });
}

function quote(arg: string): string {
  if (process.platform === 'win32') return /[\s&()^]/.test(arg) ? `"${arg}"` : arg;
  return /[^A-Za-z0-9_./=:-]/.test(arg) ? `'${arg.replace(/'/g, "'\\''")}'` : arg;
}

// Runs nova with args on the active .nova file (saved first) in the Nova terminal
async function runNova(context: vscode.ExtensionContext, args: string[]) {
  if (!vscode.workspace.isTrusted) {
    vscode.window.showWarningMessage('Nova runs programs in trusted workspaces only: trust this folder to run it.');
    return;
  }
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== 'nova') {
    vscode.window.showInformationMessage('Open a .nova file to run it.');
    return;
  }
  if (editor.document.isUntitled) {
    vscode.window.showInformationMessage('Save the file as a .nova file first.');
    return;
  }
  if (editor.document.isDirty && !(await editor.document.save())) return;
  const nova = findTool(context, 'compiler.path', 'nova');
  if (!nova) {
    const choice = await vscode.window.showErrorMessage(
      "Nova's compiler (nova) was not found. If nova.compiler.path is set, check it, or clear it to use the one " +
        'the extension ships.',
      'Open Settings');
    if (choice === 'Open Settings') vscode.commands.executeCommand('workbench.action.openSettings', 'nova.compiler.path');
    return;
  }
  const terminal = novaTerminal(novaHome(context));
  terminal.show(true);
  const file = editor.document.uri.fsPath;
  const prefix = process.platform === 'win32' && vscode.env.shell.toLowerCase().includes('powershell') ? '& ' : '';
  terminal.sendText(`${prefix}${[nova, ...args, file].map(quote).join(' ')}`);
}

async function openRepl(context: vscode.ExtensionContext) {
  if (!vscode.workspace.isTrusted) {
    vscode.window.showWarningMessage('Nova runs programs in trusted workspaces only: trust this folder to use the REPL.');
    return;
  }
  const nova = findTool(context, 'compiler.path', 'nova');
  if (!nova) {
    vscode.window.showErrorMessage("Nova's compiler (nova) was not found. If nova.compiler.path is set, check it, " +
      'or clear it to use the one the extension ships.');
    return;
  }
  const home = novaHome(context);
  const terminal = vscode.window.createTerminal({ name: 'Nova REPL', shellPath: nova, shellArgs: ['--repl'],
                                                  env: home ? { NOVA_HOME: home } : {} });
  terminal.show();
}

export async function activate(context: vscode.ExtensionContext): Promise<{ client: () => LanguageClient | undefined }> {
  output = vscode.window.createOutputChannel('Nova', { log: true });
  status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 0);
  status.command = 'nova.showOutput';
  context.subscriptions.push(output, status);

  context.subscriptions.push(
    vscode.commands.registerCommand('nova.runFile', () => runNova(context, [])),
    vscode.commands.registerCommand('nova.runFileOptimized', () => runNova(context, ['--optimize'])),
    vscode.commands.registerCommand('nova.checkFile', () => runNova(context, ['--check'])),
    vscode.commands.registerCommand('nova.openRepl', () => openRepl(context)),
    vscode.commands.registerCommand('nova.showOutput', () => output.show()),
    vscode.commands.registerCommand('nova.restartServer', async () => {
      await stopServer();
      await startServer(context);
    }),
    vscode.workspace.onDidChangeConfiguration(async e => {
      if (e.affectsConfiguration('nova.server.path') || e.affectsConfiguration('nova.home')) {
        await stopServer();
        await startServer(context);
      }
    }),
    { dispose: () => { void stopServer(); } },
  );

  await startServer(context);
  return { client: () => client };  // for the extension's tests
}

export async function deactivate(): Promise<void> {
  await stopServer();
}
