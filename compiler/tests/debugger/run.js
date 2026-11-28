#!/usr/bin/env node
// The step debugger's tests (nova --debug; the protocol is in include/debugger.h). Each session runs a program in
// this folder under the debugger, sends its commands, and compares what comes back, in order: each stop (its reason
// and every frame, innermost first, with its variables), each "input" request, and the program's output.
//
//   node tests/debugger/run.js            (the compiler: NOVA_COMPILER_PATH, or build/nova)
//
// A stop is written "reason frame | frame ...", a frame "name:line a=1 b=2" (a variable's value as the debugger
// shows it); "out: text" is a line the program printed, "input" a request for input().

const { spawnSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const HERE = __dirname;
const COMPILER_ROOT = path.resolve(HERE, "..", "..");
const NOVA = process.env.NOVA_COMPILER_PATH ||
  path.join(COMPILER_ROOT, "build", process.platform === "win32" ? "nova.exe" : "nova");

const file = name => path.join(HERE, name);

const sessions = [
  {
    name: "stepping: in, over, out, and a loop's each iteration",
    program: "loop.nova",
    commands: ["stepIn", "next", "next", "stepIn", "next", "stepOut", "next", "next", "continue"],
    expect: [
      "entry ignite:6 total=0",
      "step ignite:7 total=0 i=0",
      "step ignite:8 total=0 i=1",
      "step square:2 x=1 y=0 | ignite:8 total=0 i=1",
      "step square:3 x=1 y=1 | ignite:8 total=0 i=1",
      "step ignite:8 total=0 i=1",
      "step ignite:7 total=1 i=1",
      "step ignite:8 total=1 i=2",
      "out: 14",
    ],
  },
  {
    name: "breakpoints, recursion's frames, every kind of value, input(), and a runtime error",
    program: "values.nova",
    commands: [`breakpoints ${file("values.nova")}\t13,29`, "continue", "continue", "continue", "7", "continue"],
    expect: [
      'breakpoint ignite:29 c=Counter object v=vector<int> (2 elements) m=map<string, int> (1 element) ' +
        't=(freed after its last use) s="hi" d=2.5 ok=true f=0',
      'breakpoint fact:13 n=1 | fact:14 n=2 | fact:14 n=3 | ignite:29 c=Counter object v=vector<int> (2 elements) ' +
        'm=map<string, int> (1 element) t=(freed after its last use) s="hi" d=2.5 ok=true f=0',
      "input",
      "out: 13",
      'exception ignite:32 c=Counter object v=vector<int> (2 elements) m=map<string, int> (1 element) ' +
        't=(freed after its last use) s="hi" d=2.5 ok=true f=6 x=7 z=0',
    ],
    exitStatus: 1,
    check: stops => {
      const error = stops.find(s => s.reason === "exception");
      assert(error.text === "R0001: integer division by zero", `the error's text: ${error.text}`);
    },
  },
  {
    name: "a breakpoint in an imported module, and what an object, a vector and a tensor hold",
    program: "imports.nova",
    commands: [`breakpoints ${file("helpers.nova")}\t2`, "continue", "continue"],
    expect: [
      "breakpoint twice:2 a=3 r=0 | ignite:15 p=P object v=vector<int> (1 element) t=Tensor (2, 2) [1, 1], ... y=0",
      "out: 12",
    ],
    check: stops => {
      const [twice, ignite] = stops[0].frames;
      assert(path.basename(twice.file) === "helpers.nova" && path.basename(ignite.file) === "imports.nova",
        `each frame's own file: ${twice.file}, ${ignite.file}`);
      const vars = Object.fromEntries(ignite.variables.map(v => [v.name, v]));
      const kids = v => (v.children || []).map(c => `${c.name}=${c.value}`).join(" ");
      assert(kids(vars.p) === "x=3 w=0.5", `p's fields: ${kids(vars.p)}`);
      assert(kids(vars.v) === "[0]=4", `v's elements: ${kids(vars.v)}`);
      assert(kids(vars.t) === "[0]=[1, 1] [1]=[1, 1]", `t's rows: ${kids(vars.t)}`);
    },
  },
  {
    name: "stop ends the program where it is",
    program: "loop.nova",
    commands: ["stepIn", "stop"],
    expect: ["entry ignite:6 total=0"],
  },
];

function assert(ok, message) {
  if (!ok) throw new Error(message);
}

// What the debugger sent, in order: events (after \x1e, one per line) and the program's output lines
function run(session) {
  const result = spawnSync(NOVA, ["--debug", file(session.program)], {
    input: session.commands.join("\n") + "\n",
    encoding: "utf8",
    timeout: 20000,
    env: { ...process.env, NOVA_HOME: COMPILER_ROOT },
  });
  if (result.error) throw result.error;
  const seen = [], stops = [];
  for (const line of result.stdout.split("\n")) {
    const at = line.indexOf("\x1e");
    const out = at < 0 ? line : line.slice(0, at);
    if (out.trim()) seen.push(`out: ${out.trim()}`);
    if (at < 0) continue;
    const event = JSON.parse(line.slice(at + 1));
    if (event.event === "input") seen.push("input");
    if (event.event !== "stopped") continue;
    stops.push(event);
    const frames = event.frames.map(f =>
      [`${f.name}:${f.line}`, ...f.variables.map(v => `${v.name}=${v.value}`)].join(" "));
    seen.push(`${event.reason} ${frames.join(" | ")}`);
  }
  return { seen, stops, status: result.status, stderr: result.stderr };
}

let failed = 0;
for (const session of sessions) {
  try {
    const { seen, stops, status, stderr } = run(session);
    for (let i = 0; i < Math.max(seen.length, session.expect.length); i++) {
      assert(seen[i] === session.expect[i],
        `step ${i + 1}\n      expected: ${session.expect[i]}\n      got:      ${seen[i]}`);
    }
    assert(status === (session.exitStatus || 0), `exit status ${status}\n${stderr}`);
    if (session.check) session.check(stops);
    console.log(`  PASS  ${session.name}`);
  } catch (e) {
    failed++;
    console.log(`  FAIL  ${session.name}\n      ${e.message}`);
  }
}
console.log(`\n${sessions.length - failed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
