# Benchmarks

These numbers show how fast Nova runs today next to CPython, Node and C. Running the comparison also
turned up several correctness issues, listed below with minimal reproductions.

## Method

The suite is in [`compiler/benchmarks/compare/`](../compiler/benchmarks/compare/). Each benchmark is the same
algorithm in Nova, Python, JavaScript and C, and each program prints its result so the outputs can be
compared.

```bash
make -C compiler
python3 compiler/benchmarks/compare/run.py --optimize      # --runs N, --timeout S
```

`run.py` compiles the C files, times each program (wall clock, best of N runs, including process startup),
skips languages that aren't installed, and prints a Markdown table. It also flags runs that time out or whose
output differs from the other languages.

| Benchmark | Measures |
|---|---|
| `calls` | Naive recursive `fib(30)`, about 2.7M function calls |
| `int_loop` | A 3M-iteration `while` loop of integer `+`, `-` and `/` |
| `vector_sum` | `push` 1M ints into a `vector<int>`, then read them all back with `%` |

## Results

Measured on an Apple M4 Pro (macOS, arm64) with Python 3.11.9, Node 24.3.0 and Apple clang 17. The compiler
was built from `main` at `e2d63e6`. Times are best of 7. Nova's startup is about 3 ms.

| Benchmark | Nova | Nova `--optimize` | CPython | Node | C `-O2` |
|---|---|---|---|---|---|
| `calls` | 1,048 ms | 1,001 ms | 117 ms | 42 ms | 5 ms |
| `int_loop` | 104 ms ⚠️ | 104 ms ⚠️ | 404 ms | 48 ms | 5 ms |
| `vector_sum` | 73 ms | timed out (issue 1) | 145 ms | 48 ms | 4 ms |

⚠️ Nova prints `-562863104` here; the correct answer is `2250000000000` (issue 2).

What the numbers say:

- **Loops and vectors:** the interpreter runs straight-line integer code 2–4× faster than CPython, close to
  Node.
- **Function calls are the bottleneck:** about 9× slower than CPython. Each call in
  `runtime/interp_calls.cc`:
  - builds a `CallFrame` that heap-allocates several `std::vector`s (`savedSlots`, `allRetSlots`,
    `memo_key`);
  - copies the callee's whole slot range into the frame and back on return;
  - does hash-set lookups (`pure_functions`, `recursive_functions`, `g_tensorSlots`) on every call and for
    every parameter;
  - converts every argument through `double`.

  Moving the per-call-site decisions into the parser, and using a contiguous frame stack instead of copying
  slots in and out, should remove most of this cost. The typed SSA IR on the [roadmap](ROADMAP.md) is the
  natural place to make that change.
- **`--optimize` doesn't change these timings.** Its passes simplify the IR, but none of them reduce
  per-instruction interpreter overhead.
- **The x86 JIT and assembly back ends aren't measured.** The JIT is off by default, and neither runs on
  arm64.

### Caveats about the older benchmarks

`compiler/benchmarks/` also has an older Nova vs. Python vs. C suite (`run_benchmarks.sh`). Two of its programs
don't measure the same work in each language:

- **`fibonacci.nova`** is a pure recursive function, so Nova memoizes it automatically. `fib(35)` then computes each
  `fib(n)` once instead of making about 30 million calls, while the C and Python versions do the full recursion. Memoization is
  a real feature, but this comparison measures memoization, not the compiler. `compare/calls.nova` adds a
  never-taken `print` to switch memoization off.
- **`matrix_multiply.nova`** calls the built-in C++ `matmul`, while `matrix_multiply.py` multiplies with
  Python loops. That compares Nova's runtime library with the CPython interpreter. NumPy's `@` would be the
  fair Python baseline.

## Issues found

Each issue reproduces on `main` at `e2d63e6`.

### 1. `--optimize` never finishes on a loop that does arithmetic on a vector element

```python
ignite:
    vector<int> v = vector<int>()
    v.push(9)
    int i = 0
    int s = 0
    while (i < 1):
        s = v[i] * 7          # also with %, and with s = s + v[i] % 7
        i = i + 1
    print(s)
```

Without `--optimize` it prints `63` at once. With `--optimize` it never finishes, and memory keeps
growing: `vector_sum` with only 1,000 elements reached about 780 MB after 90 s. The same expression outside a
loop works.
CI already runs the golden tests with `--optimize`, so adding this program as a test would catch any
regression.

### 2. `int` is 32-bit and overflows silently

`int_loop` sums to 2,250,000,000,000. Nova prints `-562863104` with no warning. Options: make `int` 64-bit
(`long long` slots), trap on overflow the way Rust's debug builds do, or both.

### 3. Memoization skips side effects of the functions it calls

```python
def log(int n) -> int:
    print(n)
    return 0

def f(int n) -> int:
    int z = log(n)
    if (n < 1):
        return 0
    return f(n - 1) + f(n - 1)

ignite:
    print(f(3))
```

This should print 15 log lines, then `0`. It prints `3 2 1 0` and `0`: `f` is memoized, so the repeated
calls never reach `log`. `detect_memoizable_functions` (`runtime/memoize.cc`) only checks a function's own
instructions for `print`, input, tensor ops, array writes and allocation. It doesn't follow `CALL`s. Purity
needs to be computed over the call graph: a function is pure only if every function it calls is pure.

### 4. Training memory grows without bound

The tensor-free pass works: in `examples/xor.nova` only 8 tensors stay live. But `tensor_release`
deliberately keeps each freed `tensor_heap` entry so stale handles fail loudly, and entries are never
reused. The heap therefore grows by about 28 entries per epoch:

| XOR epochs | `tensor_heap` entries (`NOVA_TENSOR_STATS=1`) | Peak memory (RSS) |
|---|---|---|
| 1,000 | 28,012 | — |
| 5,000 | 140,012 | — |
| 10,000 | — | 75 MB |
| 50,000 | — | 312 MB |

Production workers run with `--memory=256m`, so a 50,000-epoch XOR run would be killed as out of memory, and
MNIST-sized training would hit the limit much sooner. **Generational handles** would fix this and keep the
loud failure: store a generation counter in each heap entry and in the handle (for example, the high bits),
reuse freed entries through a free list, and treat a handle whose generation doesn't match as stale.

### 5. Documented syntax that isn't implemented yet

[SYNTAX.md](SYNTAX.md) describes the target design, and its first example uses features the parser doesn't
support yet:

| Documented | Today |
|---|---|
| `10_000` digit separators | Parse error (`missing ')' after the condition of 'while'`) |
| `layer1.W.grad` | `a Tensor has no member 'grad'`; use `tensor_grad(t)` |
| `print(pred)` for a tensor | Prints the handle number; use `tensor_print(t)` |
| `"\n"`, `"\t"` escapes | Printed literally (`compiler/tests/grad_stress_loop.expected` currently expects `\n`) |

Either implement these or mark them "planned" in SYNTAX.md, so readers who copy the first example get a
program that runs.
