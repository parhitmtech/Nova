# Nova next to other ML languages and compilers

This document compares Nova with the tools people already use for machine learning. It covers which of their
ideas Nova should borrow, how each idea maps onto the [roadmap](ROADMAP.md), and where Nova should not try
to compete. Performance numbers are in [BENCHMARKS.md](BENCHMARKS.md).

## Where Nova sits

| | Nova | Python + PyTorch | JAX | Mojo | Julia |
|---|---|---|---|---|---|
| Typing | Static, checked before running | Dynamic | Dynamic; shapes fixed when traced | Optional static | Dynamic, inferred by the JIT |
| Tensors | Built-in language type | Library | Library | Library | Arrays built in; ML in libraries |
| Autodiff | Built in, at the tensor-op level | Library (eager tape) | Library (program transformation) | Library | Libraries (Zygote, Enzyme) |
| Compilation | One-pass parser → linked-list IR → interpreter, bytecode VM or x86 assembly | Eager; `torch.compile` → Triton kernels | Traced to XLA (StableHLO) | MLIR → LLVM | LLVM JIT |
| Platform | IDE, sandboxed runs, GPU cluster in one | Language and libraries | Language and libraries | Language and toolchain | Language and toolchain |
| Maturity | Research and teaching project | Industry standard | Industry standard | Young, commercial | Mature, scientific |

Two research projects matter here too:
- **Dex** (Google Research) gives arrays typed index sets.
- **Futhark** has size types and compiles to GPUs.

A third project is a cautionary tale. **Swift for TensorFlow** built differentiation into the compiler and
was archived in 2021.

## What to borrow

### 1. Tensor shapes in the type system *(builds on SYNTAX.md's planned `Tensor<float, [B, T, C]>`)*

Shape bugs are the most common ML error, and in PyTorch they only surface at runtime, often deep into
training. Python projects bolt on runtime checks (jaxtyping). Dex and Futhark show that shapes can be checked
statically. Nova is already statically typed and sees the whole program, so it can report a shape mismatch
as a compile error. A staged path:

1. Track concrete shapes where they're known: `zeros(4, 2) @ randn(3, 5)` becomes a compile error.
2. Add symbolic dimensions in signatures, `def forward(Tensor<float, [B, 2]> x) -> Tensor<float, [B, 4]>`,
   unified at each call site the way generic types already are.
3. Fall back to a runtime check only where a shape depends on data (for example, `tensor_load_csv`).

This would be the feature no mainstream ML tool offers. The planned `error_bmm_batch_mismatch` test is the
first step.

### 2. Whole-program tensor compilation *(roadmap step 3: MLIR / IREE)*

JAX and `torch.compile` both have to recover a graph from Python, by tracing or bytecode analysis, before
they can fuse and compile it. Nova doesn't: its compiler already sees every tensor op. Lowering to
StableHLO, as the roadmap plans, is the path JAX proved works. Fusing elementwise chains such as
`W - lr * dW` into one kernel also removes the intermediate tensors that dominate training memory today
(BENCHMARKS.md, issue 4).

### 3. Keep autodiff at the tensor-op level *(already the roadmap's choice)*

Swift for TensorFlow differentiated arbitrary Swift code inside the compiler. That was powerful, but it was
expensive to build and maintain. The roadmap keeps autograd at the tensor-op level and lowers both the
forward and backward graphs, the same trade-off JAX makes. If scalar-code gradients are needed later, Enzyme
differentiates at the LLVM IR level and would fit roadmap step 4.

### 4. A fast reference interpreter *(before and alongside roadmap step 4: LLVM)*

The roadmap keeps the interpreter as the reference implementation for the REPL, the debugger and tests, so
its speed matters even after LLVM arrives. Function calls cost about 9× what they cost in CPython. CPython
3.11 and Lua 5 show how far careful interpreter design goes:
- register-based instructions;
- contiguous frames instead of copying slots in and out;
- per-call-site decisions made at compile time;
- specialized ("quickened") instructions.

Most of this fits naturally once the typed SSA IR from roadmap step 2 exists.

### 5. Well-defined integer semantics

Python uses big integers, Rust traps on overflow in debug builds, and C leaves overflow undefined. Nova's
32-bit `int` wraps silently (BENCHMARKS.md, issue 2). Choose and document one rule. A 64-bit `int` that
traps on overflow in debug runs fits a language for numeric work best.

### 6. Tooling that comes with the language

Mojo, Julia and Rust ship a language server. A Nova language server (diagnostics, hover types, go to
definition), built on the parser's existing type checker, would:
- serve the Monaco editor and VS Code alike;
- give NovaBot structured compiler errors instead of text to interpret.

A formatter is the other tool these languages ship.

### 7. Differential testing and fuzzing

Nova has four back ends and an optimizer, and each one is a chance to miscompile. Production compilers catch
these bugs by running one program through every configuration and comparing outputs; this is how Csmith and
YARPGen find bugs in GCC and LLVM. Nova's golden tests already run with and without `--optimize`. Two
extensions:
- Run the same tests through the bytecode VM.
- Fuzz the lexer and parser with libFuzzer in the existing ASan + UBSan CI job.

Issues 1 and 3 in BENCHMARKS.md are the kind of bug this catches.

## Where not to compete

- **Ecosystem.** PyPI and the PyTorch ecosystem are out of reach. Interoperate instead: the `hf.*` and
  `sol.*` integrations already do this well.
- **Python compatibility.** That is Mojo's bet. Nova has its own syntax and should stay free to design it.
- **Raw speed against C.** LLVM will close much of the gap for scalar code. The compelling claim is
  *typed, compiled ML code with a cluster one function call away*, not "faster than C".

Nova's strongest position is a language and platform where ML code is type-checked, shape-checked and
compiled, and runs from the browser to a GPU cluster without leaving the IDE.
