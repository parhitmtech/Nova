// The test runner VS Code calls (extensionTestsPath): Mocha over the tests in this folder
import * as path from 'path';
import Mocha from 'mocha';

export function run(): Promise<void> {
  const mocha = new Mocha({ ui: 'tdd', color: true, timeout: 60000 });
  mocha.addFile(path.join(__dirname, 'extension.test.js'));
  return new Promise((resolve, reject) => {
    mocha.run(failures => (failures > 0 ? reject(new Error(`${failures} test(s) failed`)) : resolve()));
  });
}
