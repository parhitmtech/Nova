// The debug adapter (dist/debugAdapter.js), run alone and driven through the Debug Adapter Protocol, on the real
// compiler (NOVA_TEST_COMPILER, default compiler/build/nova) and the programs in test/fixtures.
//   npm run test:adapter
import * as assert from 'assert';
import * as path from 'path';
import { DebugClient } from '@vscode/debugadapter-testsupport';
import { DebugProtocol } from '@vscode/debugprotocol';

const root = path.resolve(__dirname, '..', '..', '..');  // editors/vscode/nova (this file runs from test/out/adapter)
const nova = process.env.NOVA_TEST_COMPILER ||
  path.resolve(root, '..', '..', '..', 'compiler', 'build', process.platform === 'win32' ? 'nova.exe' : 'nova');
const novaHome = process.env.NOVA_TEST_HOME || path.resolve(root, '..', '..', '..', 'compiler');
const fixture = (name: string) => path.join(root, 'test', 'fixtures', name);

const launch = (program: string, more: object = {}) => ({ program: fixture(program), novaPath: nova, novaHome, ...more });

suite('Nova debug adapter', () => {
  let dc: DebugClient;

  setup(() => {
    dc = new DebugClient('node', path.join(root, 'dist', 'debugAdapter.js'), 'nova');
    dc.defaultTimeout = 20000;
    return dc.start();
  });

  teardown(() => dc.stop());

  // The variables of the paused program's frame i, as "name=value"
  async function locals(frame = 0): Promise<string[]> {
    const stack = await dc.stackTraceRequest({ threadId: 1 });
    const scopes = await dc.scopesRequest({ frameId: stack.body.stackFrames[frame].id });
    const vars = await dc.variablesRequest({ variablesReference: scopes.body.scopes[0].variablesReference });
    return vars.body.variables.map(v => `${v.name}=${v.value}`);
  }

  async function where(): Promise<string[]> {
    const stack = await dc.stackTraceRequest({ threadId: 1 });
    return stack.body.stackFrames.map(f => `${f.name}:${f.line}`);
  }

  test('stops on entry, at the first statement', async () => {
    await Promise.all([
      dc.configurationSequence(),
      dc.launch(launch('debug.nova', { stopOnEntry: true })),
      dc.assertStoppedLocation('entry', { path: fixture('debug.nova'), line: 11 }),
    ]);
  });

  test('a breakpoint in a function: the call stack, and each frame its variables', async () => {
    await dc.hitBreakpoint(launch('debug.nova'), { path: fixture('debug.nova'), line: 7 });
    assert.deepStrictEqual(await where(), ['square:7', 'ignite:14']);
    assert.deepStrictEqual(await locals(0), ['n=1', 'r=0']);
    assert.deepStrictEqual(await locals(1), ['p=P object', 'total=0', 'i=1']);
  });

  test('step over, into and out', async () => {
    await Promise.all([
      dc.configurationSequence(),
      dc.launch(launch('debug.nova', { stopOnEntry: true })),
      dc.waitForEvent('stopped'),
    ]);
    const step = async (request: Promise<unknown>) => {
      await Promise.all([request, dc.waitForEvent('stopped')]);
      return (await where())[0];
    };
    assert.strictEqual(await step(dc.nextRequest({ threadId: 1 })), 'ignite:12');
    assert.strictEqual(await step(dc.nextRequest({ threadId: 1 })), 'ignite:13');
    assert.strictEqual(await step(dc.nextRequest({ threadId: 1 })), 'ignite:14');
    assert.strictEqual(await step(dc.stepInRequest({ threadId: 1 })), 'square:7');
    assert.strictEqual(await step(dc.stepOutRequest({ threadId: 1 })), 'ignite:14');
  });

  test("an object's fields, and an expression a hover or a watch asks for", async () => {
    await dc.hitBreakpoint(launch('debug.nova'), { path: fixture('debug.nova'), line: 12 });
    const stack = await dc.stackTraceRequest({ threadId: 1 });
    const scopes = await dc.scopesRequest({ frameId: stack.body.stackFrames[0].id });
    const vars = await dc.variablesRequest({ variablesReference: scopes.body.scopes[0].variablesReference });
    const p = vars.body.variables.find(v => v.name === 'p')!;
    const fields = await dc.variablesRequest({ variablesReference: p.variablesReference });
    assert.deepStrictEqual(fields.body.variables.map(v => `${v.name}=${v.value}`), ['x=3']);
    const x = await dc.evaluateRequest({ expression: 'p.x', frameId: stack.body.stackFrames[0].id, context: 'hover' });
    assert.strictEqual(x.body.result, '3');
  });

  test("the program's output, and its end", async () => {
    const output = new Promise<string>(resolve => {
      let text = '';
      dc.on('output', (e: DebugProtocol.OutputEvent) => {
        if (e.body.category === 'stdout') text += e.body.output;
        if (text.includes('5')) resolve(text);
      });
    });
    await Promise.all([dc.configurationSequence(), dc.launch(launch('debug.nova')), dc.waitForEvent('terminated')]);
    assert.strictEqual((await output).trim(), '5');
  });

  test('input(): asked for with a custom event, answered with a custom request', async () => {
    const asked = dc.waitForEvent('nova.input');
    await Promise.all([dc.configurationSequence(), dc.launch(launch('input.nova'))]);
    await asked;
    const output = new Promise<string>(resolve => {
      dc.on('output', (e: DebugProtocol.OutputEvent) => { if (e.body.output.includes('42')) resolve(e.body.output); });
    });
    await dc.customRequest('nova.input', { value: '21' });
    assert.strictEqual((await output).trim(), '42');
  });

  test('Pause stops a running program, even in a loop on one line', async () => {
    await Promise.all([dc.configurationSequence(), dc.launch(launch('spin.nova'))]);
    await new Promise(r => setTimeout(r, 300));  // it runs
    const stopped = dc.waitForEvent('stopped') as Promise<DebugProtocol.StoppedEvent>;
    await dc.pauseRequest({ threadId: 1 });
    assert.strictEqual((await stopped).body.reason, 'pause');
    assert.deepStrictEqual(await where(), ['ignite:3']);
    assert.match((await locals())[0], /^n=\d+$/);
  });

  test('a breakpoint set while the program runs stops it', async () => {
    await Promise.all([dc.configurationSequence(), dc.launch(launch('count.nova'))]);
    await new Promise(r => setTimeout(r, 300));  // it runs, with no breakpoint
    const stopped = dc.waitForEvent('stopped') as Promise<DebugProtocol.StoppedEvent>;
    await dc.setBreakpointsRequest({ source: { path: fixture('count.nova') }, breakpoints: [{ line: 4 }] });
    assert.strictEqual((await stopped).body.reason, 'breakpoint');
    assert.deepStrictEqual(await where(), ['ignite:4']);
  });

  test('a runtime error stops the program there, with its message', async () => {
    const stopped = dc.waitForEvent('stopped') as Promise<DebugProtocol.StoppedEvent>;
    await Promise.all([dc.configurationSequence(), dc.launch(launch('divide.nova'))]);
    const e = await stopped;
    assert.strictEqual(e.body.reason, 'exception');
    assert.strictEqual(e.body.text, 'R0001: integer division by zero');
    assert.deepStrictEqual(await where(), ['ignite:3']);
    assert.deepStrictEqual(await locals(), ['z=0']);
    const exited = dc.waitForEvent('exited') as Promise<DebugProtocol.ExitedEvent>;
    await dc.continueRequest({ threadId: 1 });
    assert.strictEqual((await exited).body.exitCode, 1);
  });
});
