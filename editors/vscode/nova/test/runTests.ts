// Starts VS Code (downloaded once into .vscode-test/) with this extension loaded, opens test/fixtures, and runs the
// tests in suite/. The extension is pointed at the compiler's build (NOVA_BUILD, default compiler/build).
//   VSCODE_VERSION: the VS Code to test with; default the oldest the extension supports (engines.vscode)
import * as path from 'path';
import { runTests } from '@vscode/test-electron';

async function main() {
  const root = path.resolve(__dirname, '..', '..');                       // editors/vscode/nova
  const build = process.env.NOVA_BUILD || path.resolve(root, '..', '..', '..', 'compiler', 'build');
  const exe = (name: string) => path.join(build, process.platform === 'win32' ? `${name}.exe` : name);
  const pkg = require(path.join(root, 'package.json'));
  const version = process.env.VSCODE_VERSION || String(pkg.engines.vscode).replace(/^\^/, '');
  try {
    const code = await runTests({
      version,
      // NOVA_TEST_EXTENSION_PATH: an unpacked package to test instead of this folder
      extensionDevelopmentPath: process.env.NOVA_TEST_EXTENSION_PATH || root,
      extensionTestsPath: path.join(__dirname, 'suite', 'index'),
      launchArgs: [path.join(root, 'test', 'fixtures'), '--disable-extensions', '--disable-workspace-trust'],
      extensionTestsEnv: {
        NOVA_TEST_BUNDLED: process.env.NOVA_TEST_BUNDLED,
        NOVA_TEST_SERVER: exe('nova-lsp'),
        NOVA_TEST_COMPILER: exe('nova'),
        NOVA_TEST_HOME: path.resolve(build, '..'),
      },
    });
    process.exit(code);
  } catch (e) {
    console.error('The extension tests did not run:', e);
    process.exit(1);
  }
}

main();
