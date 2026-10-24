#!/usr/bin/env python3
"""Like-for-like benchmarks: Nova vs. CPython vs. Node vs. C.

Usage (from the repo root, after `make -C compiler`):
    python3 compiler/benchmarks/compare/run.py [--runs N] [--timeout S] [--optimize]

Every benchmark is the same algorithm in each language, and every program prints
its result so the outputs can be compared. Times are wall-clock, best of N runs,
and include process startup. Languages whose tool is not installed are skipped.
A C compiler builds the .c files into a temporary directory.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
COMPILER_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
NOVA = os.environ.get("NOVA_COMPILER_PATH", os.path.join(COMPILER_ROOT, "build", "nova"))
BENCHMARKS = ["calls", "int_loop", "vector_sum"]


def best_time(cmd, runs, timeout):
    """Return (best milliseconds, last line of output), or (None, reason)."""
    best, out = None, ""
    for _ in range(runs):
        start = time.perf_counter()
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None, f"timed out after {timeout}s"
        elapsed = (time.perf_counter() - start) * 1000
        lines = (proc.stdout or proc.stderr).strip().splitlines()
        out = lines[-1] if lines else ""
        if proc.returncode != 0:
            return None, f"exit {proc.returncode}: {out}"
        best = elapsed if best is None else min(best, elapsed)
    return best, out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--optimize", action="store_true", help="also time Nova with --optimize")
    args = parser.parse_args()

    if not os.path.exists(NOVA):
        sys.exit(f"Nova compiler not found at {NOVA}; run `make -C compiler` first.")

    cc = shutil.which(os.environ.get("CC", "cc"))
    build_dir = tempfile.mkdtemp(prefix="nova-bench-")
    os.environ["NOVA_HOME"] = COMPILER_ROOT

    def src(bench, ext):
        return os.path.join(HERE, f"{bench}.{ext}")

    columns = [("Nova", lambda b: [NOVA, src(b, "nova")])]
    if args.optimize:
        columns.append(("Nova --optimize", lambda b: [NOVA, src(b, "nova"), "--optimize"]))
    if shutil.which("python3"):
        columns.append(("CPython", lambda b: ["python3", src(b, "py")]))
    if shutil.which("node"):
        columns.append(("Node", lambda b: ["node", src(b, "js")]))
    if cc:
        columns.append(("C -O2", lambda b: [os.path.join(build_dir, b)]))

    print(f"| Benchmark | {' | '.join(name for name, _ in columns)} |")
    print(f"|---|{'---|' * len(columns)}")
    notes = []
    for bench in BENCHMARKS:
        if cc:
            subprocess.run([cc, "-O2", "-o", os.path.join(build_dir, bench), src(bench, "c")], check=True)
        cells, outputs = [], {}
        for name, make_cmd in columns:
            ms, out = best_time(make_cmd(bench), args.runs, args.timeout)
            cells.append(f"{ms:.0f} ms" if ms is not None else "—")
            if ms is None:
                notes.append(f"{bench} / {name}: {out}")
            else:
                outputs[name] = out
        print(f"| `{bench}` | {' | '.join(cells)} |")
        if len(set(outputs.values())) > 1:
            notes.append(f"{bench}: outputs differ: " + ", ".join(f"{k}={v}" for k, v in outputs.items()))

    shutil.rmtree(build_dir, ignore_errors=True)
    for note in notes:
        print(f"\n- {note}", end="")
    print()


if __name__ == "__main__":
    main()
