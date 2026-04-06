#!/usr/bin/env node
//
// NovaComp Test Runner
// Usage:  node run_tests.js [filter]
//   filter  optional substring to match test file names, e.g. node run_tests.js queue
//
// Test layout (all inside ./tests/):
//   foo.nova          — source file
//   foo.expected      — expected stdout, one value per line
//   foo.input         — (optional) stdin inputs, one value per line
//

const { spawn }   = require("child_process");
const fs          = require("fs");
const path        = require("path");

const BINARY      = path.resolve(__dirname, process.platform === "win32" ? "./compiler.exe" : "./compiler");
const TESTS_DIR   = path.resolve(__dirname, "tests");
const TIMEOUT_MS  = 10000;
const filter      = process.argv[2] || "";

// ── helpers ───────────────────────────────────────────────────────────────────

// Temp files must live inside the project dir so stdlib/ imports resolve correctly.
const TMP_FILE = path.resolve(__dirname, "_test_run_.csl");

function writeTempFile(source) {
  fs.writeFileSync(TMP_FILE, source + "\n", "utf8");
  return { tmpDir: null, tmpFile: TMP_FILE };
}

function cleanup(tmpFile, _tmpDir) {
  try { fs.unlinkSync(tmpFile); } catch (_) {}
}

function runCompiler(source, inputs) {
  return new Promise((resolve) => {
    const { tmpDir, tmpFile } = writeTempFile(source);
    let stdout = "", stderr = "";

    const child = spawn(BINARY, [tmpFile], {
      stdio: ["pipe", "pipe", "pipe"],
      env: { ...process.env, NOVA_HOME: __dirname },
    });
    child.stdout.on("data", d => { stdout += d.toString(); });
    child.stderr.on("data", d => { stderr += d.toString(); });

    if (inputs && inputs.trim().length > 0)
      child.stdin.write(inputs.trim() + "\n");
    child.stdin.end();

    const timer = setTimeout(() => {
      child.kill();
      cleanup(tmpFile, tmpDir);
      resolve({ ok: false, reason: "TIMEOUT" });
    }, TIMEOUT_MS);

    child.on("close", () => {
      clearTimeout(timer);
      cleanup(tmpFile, tmpDir);
      if (stderr.trim()) resolve({ ok: false, reason: "COMPILER_ERROR", detail: stderr.trim() });
      else               resolve({ ok: true,  output: stdout });
    });
  });
}

function normalise(s) {
  // Trim each line and drop trailing blank lines so formatting doesn't matter.
  return s.split("\n").map(l => l.trim()).filter((l, i, a) => {
    // keep interior blank lines but drop trailing ones
    if (l === "") return i < a.length - 1 && a.slice(i + 1).some(x => x !== "");
    return true;
  }).join("\n");
}

// ── collect tests ─────────────────────────────────────────────────────────────

const novaFiles = fs.readdirSync(TESTS_DIR)
  .filter(f => f.endsWith(".nova") && (!filter || f.includes(filter)))
  .sort();

if (novaFiles.length === 0) {
  console.log("No test files found" + (filter ? ` matching '${filter}'` : "") + " in ./tests/");
  process.exit(0);
}

// ── run tests ─────────────────────────────────────────────────────────────────

const GREEN  = "\x1b[32m";
const RED    = "\x1b[31m";
const YELLOW = "\x1b[33m";
const RESET  = "\x1b[0m";
const BOLD   = "\x1b[1m";

let passed = 0, failed = 0, skipped = 0;

(async () => {
  console.log(`\n${BOLD}NovaComp Test Runner${RESET}  (${novaFiles.length} test${novaFiles.length !== 1 ? "s" : ""})\n`);

  for (const file of novaFiles) {
    const base     = file.replace(/\.nova$/, "");
    const srcPath  = path.join(TESTS_DIR, file);
    const expPath  = path.join(TESTS_DIR, base + ".expected");
    const inpPath  = path.join(TESTS_DIR, base + ".input");

    if (!fs.existsSync(expPath)) {
      console.log(`  ${YELLOW}SKIP${RESET}  ${base}  (no .expected file)`);
      skipped++;
      continue;
    }

    const source   = fs.readFileSync(srcPath,  "utf8");
    const expected = fs.readFileSync(expPath,  "utf8");
    const inputs   = fs.existsSync(inpPath) ? fs.readFileSync(inpPath, "utf8") : "";

    const result = await runCompiler(source, inputs);

    if (!result.ok) {
      const detail = result.reason === "TIMEOUT"
        ? "execution timed out"
        : result.detail.split("\n")[0];
      console.log(`  ${RED}FAIL${RESET}  ${base}`);
      console.log(`        ${RED}→ ${detail}${RESET}`);
      failed++;
      continue;
    }

    const got  = normalise(result.output);
    const want = normalise(expected);

    if (got === want) {
      console.log(`  ${GREEN}PASS${RESET}  ${base}`);
      passed++;
    } else {
      console.log(`  ${RED}FAIL${RESET}  ${base}`);
      const wantLines = want.split("\n");
      const gotLines  = got.split("\n");
      const maxLen    = Math.max(wantLines.length, gotLines.length);
      for (let i = 0; i < maxLen; i++) {
        const w = wantLines[i] ?? "(missing)";
        const g = gotLines[i]  ?? "(missing)";
        if (w !== g)
          console.log(`        line ${i+1}: expected ${BOLD}${w}${RESET}  got ${RED}${g}${RESET}`);
      }
      failed++;
    }
  }

  console.log(`\n${BOLD}Results:${RESET}  ${GREEN}${passed} passed${RESET}  ${failed > 0 ? RED : ""}${failed} failed${RESET}  ${skipped > 0 ? YELLOW : ""}${skipped} skipped${RESET}\n`);
  process.exit(failed > 0 ? 1 : 0);
})();
