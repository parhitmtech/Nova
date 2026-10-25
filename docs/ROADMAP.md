# Roadmap

Where NovaComp is heading after the syntax redesign (steps 1–9 and stages A1–A3 are done; see
[SYNTAX.md](SYNTAX.md)). Items are in order: each one builds on the ones before it.

## 1. Finish the language

- **B: objects as references.** Objects get a handle into a runtime object heap, like Python, Java and C#, in
  place of flattened fields with copy-in/copy-out on every method call. Then objects can be passed,
  returned, stored in fields and containers (`vector<P>`), and used as type arguments (`Box<P>`).
  - B1 (done): the object heap; fields and methods go through the handle. An object cannot be shared yet,
    so it is freed when its declaration runs again or its function returns.
  - B2 (done): objects as parameters, return values and in assignment (`q = p` shares the object), with
    reference counting: each call owns the references its variables, parameters and temporaries hold.
  - B3 (done): objects in fields and containers, and class type arguments (`Box<P>`, `vector<P>`); `.x` /
    `.m()` on any object value (`make().x`, `self.area()`, `return self`); recursive methods.
  - B4 (done): containers are reference-counted as objects are (a call's variables, parameters and
    temporaries, and object fields each hold a reference), and freed with their elements' references when
    nothing refers to them, so a `vector` made in a loop no longer keeps its elements alive.
  - `none` (done): an empty value for object fields and variables, tested with `==` / `!=` (identity), so a
    linked list ends at `none` instead of keeping its own size.
  - Next: passing a subclass where its parent is expected, with methods chosen by the object's own class at
    run time (virtual methods).
  - Small gaps found on the way: freeing an object's arrays (array slots are never reclaimed); a cycle of objects (a node referring back
    to its parent) is never freed; a method can call methods defined before it, and itself, but not later ones.
- **Batched matmul (`bmm`)**, with its tests (`tensor_bmm`, `grad_bmm`, `error_bmm_batch_mismatch`).

## 2. A real middle-end IR ("Nova IR")

Today the parser's linked-list IR is also the interpreter's format: it is slot-numbered and mixes three memory
spaces (`mem`, `fmem`, `dmem`). Add a typed SSA IR between the parser and the back ends, with explicit tensor
values and ops, that the interpreter can still run. This boundary is what makes the next two steps simple
lowerings instead of rewrites, and it is independent of any outside library.

## 3. Tensors on GPUs: MLIR and IREE

- Lower Nova IR's tensor ops (and the backward graph that autograd builds) to StableHLO or MLIR `linalg`.
- Compile with **IREE** (an MLIR-based ML compiler and runtime) for CPU first, then CUDA on the SOL / AWS GPUs.
  This gives fused, tiled kernels and cuBLAS-class matmul without hand-written kernels.
- Start by emitting StableHLO text and calling the prebuilt `iree-compile`; link IREE's C++ APIs only once that
  path works. Pin one release.
- Autograd stays at the tensor-op level in Nova (MLIR has no built-in autodiff); both the forward and the
  backward graph are lowered.

## 4. Scalar code: LLVM

- Lower the rest of Nova IR (loops, ints, calls, objects) to LLVM IR, directly or through MLIR's LLVM dialect so
  there is one toolchain.
- This replaces the experimental x86 JIT (off by default; it miscompiles general code) and the x86 assembly
  emitter, and gives native code on ARM (AWS Graviton) as well as x86.
- The interpreter stays as the reference implementation and runs the REPL and debugger.

## What stays our own

The lexer, parser, type system, generics, Nova IR and the tensor/autograd design are NovaComp's own work.
Register allocation, instruction selection, vectorization and GPU kernel generation come from LLVM / MLIR /
IREE instead of being written by hand.

## Costs to plan for

LLVM and MLIR are large C++ dependencies (multi-GB builds, APIs that change between releases). Pin a prebuilt
release in CI. IREE adds its own runtime. Linux (CI, AWS) is the main target; Windows builds are harder.

## Platform follow-ups

- A refresh flow for the 1-hour session token (users sign in again every hour today).
- The deploy workflow's EC2 host and user secrets, and HTTPS-only `NOVA_ALLOWED_ORIGINS` on the host, before
  the first automatic deploy.
