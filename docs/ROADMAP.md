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
  - `--optimize` hangs (or crashes) while compiling a loop nested in another whose body uses the outer loop's
    variable times a constant (`s = s + i * 7` inside `while (j < 2)` inside `while (i < 2)`); the hang is in the
    optimizer, before `--dump-ir` prints, most likely loop-invariant code motion.
  - `--dump-ir` prints `strMem[mem[slot]]` for strings that only exist at run time, so on some programs it
    prints garbage or stops partway (the dump only: the programs run correctly).

## 2. A production compiler (phases 1–7)

Before the ML work, the front end, the runtime's memory and the middle-end are rebuilt the way production
compilers build them (Clang, rustc, Go, Zig, Carbon, CPython, Lua). The x86 assembly emitter, the
experimental x86 JIT and the `.nbc` bytecode export (with its standalone VM and the IDE's export) were removed
first: they could not keep up with the language, and native code will come from LLVM (section 4).

**The front end**

1. **Lexer** (done). A pointer-based scanner over one source buffer writes compact tokens (a kind, a 32-bit
   location in one space over all buffers, a length, an interned symbol); names and keywords share one
   interned table (a keyword is recognised by the same lookup); NEWLINE / INDENT / DEDENT are made while
   scanning. `#@line` markers from imports become line directives in the SourceManager, which works out lines
   and columns only when asked. The current parser reads these tokens through a small compatibility pass that
   rebuilds the braces-and-semicolons form; phase 2's parser reads them directly.
2. **Parse tree and check pass.** The parser builds a flat tree (Carbon / Zig style) and a separate pass
   resolves names and types and lowers to IR, replacing the one-pass parser's global state (`lastExprType`
   etc.) and the token re-reading for generics. One interned symbol table with scopes as a stack replaces the
   ten name-keyed maps. `+=` and friends become real operators (today `v[f()] += 1` evaluates `f()` twice).
   - 2a (done): the parser reads the scanner's tokens and builds the tree (a postorder array of 12-byte nodes),
     deciding every construct from the tokens alone: `Point p` is a declaration because a name follows a type, and
     `Box<int>(5)` a call because `(` follows the type arguments. `--dump-tree` prints it; nothing is compiled from
     it yet. It parses 100,000 lines in about 5 ms.
   - 2b: the check pass over the tree, in three steps. 2b-1 (done): names and scopes. One symbol table indexed by
     interned name, with each binding hiding the one before it, so leaving a scope pops its bindings; the result is
     an entity for every definition and variable and, for each node, the entity it names. Every definition is
     known before any body is checked. The check pass runs before the old parser and reports name errors
     (undeclared, redeclared, unknown types, break / return / self out of place, the entry point). 2b-2: types of
     expressions, calls, containers and objects. 2b-3: generic functions and classes.
   - 2c: lowering from the checked tree to the IR; the old parser and the braces-form pass are removed.
3. **Error recovery.** Error nodes and resynchronisation at statement and block boundaries, so one mistake gives
   one error; messages give the column as well as the line (from the token locations phase 1 records).

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
