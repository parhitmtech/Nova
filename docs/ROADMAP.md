# Roadmap

Where NovaComp is heading after the syntax redesign (steps 1–9, stages A and B are done; see
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
  - Subclasses (done): a subclass goes wherever its parent is expected, with methods chosen by the object's
    own class at run time (virtual methods).
  - Small gaps found on the way: a cycle of objects (a node referring back to its parent) is never freed; a
    method can call methods defined before it, and itself, but not later ones. Both are covered by the phases
    below (weak fields in phase 5; the check pass in phase 2).

## 2. A production compiler (phases 1–7)

Before the ML work, the front end, the runtime's memory and the middle-end are rebuilt the way production
compilers build them (Clang, rustc, Go, Zig, Carbon, CPython, Lua). The x86 assembly emitter, the
experimental x86 JIT and the `.nbc` bytecode export (with its standalone VM and the IDE's export) were removed
first: they could not keep up with the language, and native code will come from LLVM (section 4).

**The front end**

1. **Lexer.** Tokens are a kind plus a span into the one source buffer (no copied strings); one 32-bit
   location space over all files, with line and column worked out only when an error is printed (no line
   offsets for imports); names interned in one table that also recognises keywords; INDENT / DEDENT made while
   scanning; `+=` and friends as real operators (today `v[f()] += 1` evaluates `f()` twice).
2. **Parse tree and check pass.** The parser builds a flat tree (Carbon / Zig style) and a separate pass
   resolves names and types and lowers to IR, replacing the one-pass parser's global state (`lastExprType`
   etc.) and the token re-reading for generics. One interned symbol table with scopes as a stack replaces the
   ten name-keyed maps.
3. **Error recovery.** Error nodes and resynchronisation at statement and block boundaries, so one mistake gives
   one error.

**Memory and the middle-end**

4. **Strings** are reference-counted and freed (today every concatenation stays in memory for good).
5. **Arrays** become counted heap values like containers; **`weak` fields** break cycles (Swift-style), so a
   tree whose children point back to their parent is freed.
6. **Frames on a growable value stack** (CPython 3.11 / Lua style): a variable is an offset from its frame, so a
   call no longer copies the function's slots in and out; recursion depth is bounded by memory, with a clean
   "recursion too deep" error at a set limit.
7. **Nova IR**, a typed IR between the check pass and the back ends, built on phase 6's frame model, that the
   interpreter runs. It replaces the slot-numbered linked list that mixes three memory spaces (`mem`, `fmem`,
   `dmem`), and it is what the ML work and LLVM lower from.

## After phase 7: the ML work

Planned in detail once phase 7 is done. It starts with **batched matmul (`bmm`)**, with its tests (`tensor_bmm`,
`grad_bmm`, `error_bmm_batch_mismatch`), and then the tensor compiler below, custom CUDA / cuBLAS kernels,
memory (out-of-memory) work, and FlashAttention / Triton-style inference.

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
- This gives native code (in place of the removed x86 JIT and assembly emitter) on ARM (AWS Graviton) as well
  as x86.
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
