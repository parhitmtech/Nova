# NovaComp

[![CI](https://github.com/parhitmtech/Nova/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/parhitmtech/Nova/actions/workflows/ci.yml)

**Nova** is a statically typed programming language for machine learning, with Python-style layout and
tensors, autograd and GPU-cluster training built into the language. **NovaComp** is the cloud IDE around
it: a browser editor that compiles and runs Nova in sandboxed containers, streams the output back, and
includes an AI coding assistant.

```python
orbit novatorch.nn

def scale(x, k):                  # no types written: compiled once per argument-type combination
    return x * k

ignite:                           # the entry point
    Linear layer = Linear(2, 3)
    Tensor h = relu(layer.forward(randn(4, 2)))
    print(h.rows)                 # 4

    Tensor W = randn(3, 2)
    requires_grad(W)
    Tensor loss = mse_loss(ones(4, 3) @ W, zeros(4, 2))
    backward(loss)                # reverse-mode autograd
    print(tensor_grad(W).rows)    # 3

    print(scale(1.5, 2.0))        # 3.000000
```

## Why Nova

Python is the language of machine learning, but it's dynamically typed and slow, so the heavy lifting
happens in C++/CUDA libraries the language can't see into. Nova makes tensors a built-in type that the compiler
understands, which leaves room for what a library can't do: type-check tensor code before it runs, fuse
and compile tensor ops, and run the same program on a laptop or a GPU cluster.

## What's in the box

| Part | What it does |
|---|---|
| **Language** | Indentation-based syntax; static types (`int`, `float`, `double`, `bool`, `string`, `Tensor`); functions with several return values; generic functions and classes; structs, classes and inheritance; `vector<T>` and `map<K, V>`; modules via `orbit` |
| **Tensors and ML** | 1–6-dimensional tensors with `+ - * / @`, indexing and slicing, activations, losses, reverse-mode autograd, and `novatorch.nn` / `optim` / `data` in the standard library |
| **Compiler** | C++17; one-pass type-checking parser; optional optimizations (constant folding, copy propagation, dead-code elimination, loop-invariant code motion, inlining); a compiler pass that frees each tensor after its last use; an interpreter, a bytecode VM, x86-64 assembly with Chaitin-Briggs register allocation, and CUDA source output |
| **IDE** | React + Monaco editor, file workspace, terminal, REPL, step debugger, sharing, and NovaBot (Claude via AWS Bedrock), which can read, write and run code |
| **Execution** | Each run gets its own network-less, read-only container with CPU, memory, time and process limits |
| **GPU training** | `sol.finetune(...)` submits HuggingFace training jobs to ASU's SOL cluster over SLURM |

## Quick start

You need `make`, a C++17 compiler (g++ or clang++) and Node.js 22.12+.

```bash
git clone https://github.com/parhitmtech/Nova.git && cd Nova
make -C compiler                      # builds compiler/build/nova
make -C compiler test                 # the golden language tests

export NOVA_HOME=$PWD/compiler        # where `orbit` finds the standard library
compiler/build/nova examples/xor.nova # trains a 2-layer network on XOR
```

Useful compiler flags: `--check` (type-check only), `--optimize`, `--dump-ir`, `--debug`, `--repl`,
`--emit-asm`, `--emit-bytecode -o out.nbc`, `--emit-cuda`.

To run the whole IDE locally (backend, frontend, Docker workers), follow [docs/SETUP.md](docs/SETUP.md).

## How Nova compares

| | Nova | Python + PyTorch | JAX | Mojo | Julia |
|---|---|---|---|---|---|
| Typing | Static, checked before running | Dynamic | Dynamic (traced) | Static where declared | Dynamic, inferred |
| Tensors | Built-in type | Library | Library | Library | Library (arrays built in) |
| Autodiff | Built in | Library (autograd) | Library (`grad`) | Library | Library (Zygote, Enzyme) |
| Execution | Interpreter; bytecode VM; x86 assembly | Interpreter + C++/CUDA kernels | XLA compiler | MLIR → native | LLVM JIT |
| Comes with an IDE, sandbox and GPU cluster | Yes | No | No | No | No |

Nova's lead is integration: one language, editor, sandbox and GPU path. It's a young, single-backend
language and doesn't compete on ecosystem or raw speed yet. [docs/COMPARISON.md](docs/COMPARISON.md) covers
where it fits and which ideas from these projects it should borrow.

## Performance

These are like-for-like programs that print the same result in every language. Times are best of 7 runs,
including process startup, on an Apple M4 Pro.

| Benchmark | Nova | CPython 3.11 | Node 24 | C (clang -O2) |
|---|---|---|---|---|
| `calls`: naive `fib(30)`, 2.7M calls | 1,048 ms | 117 ms | 42 ms | 5 ms |
| `int_loop`: 3M-iteration arithmetic loop | 104 ms ⚠️ | 404 ms | 48 ms | 5 ms |
| `vector_sum`: fill and sum 1M-element vector | 73 ms | 145 ms | 48 ms | 4 ms |

Nova's interpreter is 2–4× faster than CPython on loops and vectors, but about 9× slower on function
calls. ⚠️ `int_loop` gives the wrong answer because Nova's `int` is 32-bit and overflows silently. See
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) for the method, caveats such as automatic memoization, and the
issues the benchmarks found. To reproduce:

```bash
python3 compiler/benchmarks/compare/run.py --optimize
```

## Status

Nova is in active development. The syntax redesign and container stage A are done. Next on the
[roadmap](docs/ROADMAP.md): objects as references, a typed SSA middle-end IR, tensor compilation through
MLIR/IREE, and LLVM for scalar code, which brings native ARM and x86. Known gaps today:

- An object can't yet be passed, returned, or stored in a field or container (stage B).
- Some of [docs/SYNTAX.md](docs/SYNTAX.md) is still the *target* design. Not yet implemented: `10_000`
  literals, `t.grad`, `\n` / `\t` escapes, and `print(t)` for tensors. Use `tensor_print(t)` for now.
- The x86 JIT is experimental and is off by default.

## Documentation

| Doc | For |
|---|---|
| [docs/SYNTAX.md](docs/SYNTAX.md) | The Nova language |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the IDE, backend, sandbox, NovaBot, SOL and compiler fit together |
| [docs/SETUP.md](docs/SETUP.md) | Running and testing everything locally |
| [docs/DEPLOY.md](docs/DEPLOY.md) | Production deployment on EC2 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What comes next |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Performance numbers, method, and the issues they found |
| [docs/COMPARISON.md](docs/COMPARISON.md) | Nova next to Python/PyTorch, JAX, Mojo, Julia and others |

## Repository layout

```
compiler/    the Nova language: C++17 compiler, runtime, stdlib, tests, benchmarks
server/      Node.js + Express backend: auth, execution, workspace, NovaBot
frontend/    React + Vite IDE
examples/    Nova example programs
docs/        the documents above
deploy/      Docker Compose and deploy script for the EC2 host
tools/       SOL tunnel scripts, syntax migration scripts
```

## Contributing

Every pull request runs the 8 required CI checks in [.github/workflows/ci.yml](.github/workflows/ci.yml).
Before you push, run the language tests with and without the optimizer:

```bash
make -C compiler test
NOVA_TEST_ARGS=--optimize make -C compiler test
```

A language change ships with a golden test in `compiler/tests/` (`name.nova` plus `name.expected`, or
`name.error` for a compile error) and an update to [docs/SYNTAX.md](docs/SYNTAX.md).
