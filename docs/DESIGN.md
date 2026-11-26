# Design record

The decisions behind Nova's compiler and kernels: for each, what was chosen, what else was possible, and why. The
[roadmap](ROADMAP.md) says what comes next; each step adds its decisions here.

## Front end

### 1. A flat parse tree in postorder

**Decision.** The parser writes the tree as one array of 12-byte nodes in postorder (a node's children come
before it), each holding its kind, its token and the size of its subtree. Children are found by walking back from a
node, not through pointers.

**Alternatives.** A pointer-based tree (a heap object per node, child pointers), as most textbook compilers and
the old parser's ad-hoc state used.

**Why.** One allocation for the whole tree; nodes sit in memory in the order a pass visits them, so walks are cache
friendly; a subtree is a contiguous range, so "every node of this function" is a slice. 100,000 lines parse in
about 5 ms. Carbon and Zig use the same layout.

**Cost.** Editing the tree in place is awkward (a rewrite means a new array), and code reads children through
helpers instead of fields. Nova's passes read the tree and write new structures, so that cost is never paid.

### 2. Tokens that point into the source

**Decision.** A token is a kind, a 32-bit location in one space over every source buffer, a length and an
interned symbol: no copy of its text. Lines and columns are worked out from the location only when an error needs
them, and INDENT / DEDENT are made while scanning.

**Alternatives.** Tokens holding their own strings and line numbers (the old lexer).

**Why.** Small, fixed-size tokens; names compare as integers once interned; keywords are found by the same lookup
as names. One location space lets an imported file's tokens carry their own positions.

### 3. A check pass separate from lowering

**Decision.** Names, scopes and types are decided by a check pass over the tree, which writes an entity for every
definition and the type of every expression. Lowering reads that result and decides nothing about meaning.

**Alternatives.** A one-pass compiler that type-checks while it emits code, which the old parser was.

**Why.** Each pass has one job and can be tested on its own (the `error_*` tests are check-pass tests). Every
definition is known before any body is checked, so a function can be called above its definition, and mutually
recursive functions need no forward declarations. The one-pass parser kept its state in about ten global maps
keyed by name, and re-read tokens to compile generics.

### 4. Unknown fits everywhere

**Decision.** A value whose type cannot be known yet (a method's untyped parameter, the result of a method without
`-> type`) has type Unknown, which is accepted wherever a type is expected.

**Alternatives.** Reject such programs, or infer through method bodies.

**Why.** It let the check pass land one language feature at a time without rejecting programs that worked, and it
marks exactly where the type system is still incomplete.

**Cost.** An Unknown hides mistakes the check pass could catch; the aim is to remove the places it can arise.

### 5. Generics compiled once per type, as copies

**Decision.** A call to a generic function makes a copy for its argument types (`scale<int, int>`), and writing
`Box<int>` makes Box's copy for `int`. Each copy is checked and lowered with concrete types, as its own function or
class.

**Alternatives.** Type erasure (one compiled body working on boxed values, as Java does) or dictionary passing
(one body taking the operations as hidden arguments, as Haskell's type classes do).

**Why.** Every copy has concrete types, so it is lowered and optimized like non-generic code and pays nothing at
run time. This is how C++ templates and Rust generics work.

**Cost.** Code size grows with the number of copies, and each copy is checked again.

### 6. Replacing the parser one step at a time

**Decision.** The new front end lowered into the existing IR, one language area per step (scalars, tensors, structs
and classes, containers, generics). While lowering covered only part of the language, the driver lowered what it
covered and gave the rest to the old parser; the old parser was deleted only when nothing reached it.

**Alternatives.** Rewrite the front end and the IR together and switch over at once.

**Why.** The optimizer, the interpreter and the CUDA back end did not change, so every test compared the old and new
front ends on the same IR at every step. The strangler-fig pattern: the new system grows around the old one until
the old one can go.

## Runtime

### 7. Reference counting for objects and containers

**Decision.** Objects, vectors and maps live in heaps and are reference-counted: a call's variables, parameters and
temporaries, and object fields, each hold a reference, and a value is freed when its count reaches 0.

**Alternatives.** A tracing garbage collector, or copying objects by value.

**Why.** Freeing happens at a known point, which an interpreter, a REPL and a debugger can show, and it needs no
collector. Swift and CPython make the same choice.

**Cost.** Cycles (a node pointing back to its parent) are never freed; `weak` fields, as in Swift, are on the
roadmap.

### 8. Tensors freed by liveness, at compile time

**Decision.** A compiler pass computes liveness over the IR and inserts a release after each tensor's last use.

**Alternatives.** Reference-count tensors at run time on every copy, or never free them.

**Why.** Training loops make many temporary tensors; freeing them where liveness says they die keeps memory flat
without a check on every copy.

### 9. Batched matmul as a plan of matrix indices

**Decision.** `matmul_plan` works out, for each matrix product of a batched matmul, which matrix of A and which of
B it uses. Broadcasting is a repeated index; the forward pass reads through the plan and the backward pass adds
into it.

**Alternatives.** Copy the broadcast operand out to the full batch, then multiply.

**Why.** No copies, and a matrix shared by several products gets the sum of their gradients for free. The CUDA
kernels and cuBLAS express the same thing with a batch stride of 0.

## Optimizer

### 10. Loop-invariant code motion that cannot change a program

**Decision.** An instruction leaves a loop only if it is an integer `+`, `-` or `*` whose operands the loop never
writes, its result is written once in the loop, is not live at the loop's header and is not read after the loop,
and the loop is entered only through its header.

**Alternatives.** Hoist every computation with invariant operands.

**Why.** The IR names memory slots, not SSA values, and slots are written many times; hoisting naively changes what
a program prints (a value read before the loop writes it, a loop that runs no times, a division moved onto a path
that never divided). The first version linked the instruction list into a cycle; `licm_loops` covers each rule.
On an SSA IR (roadmap step C) most of these conditions hold by construction.

## Kernels

### 11. GPU matmul by hand, in four steps

**Decision.** Four CUDA kernels, each removing one named bottleneck: naive (memory bound), shared-memory tiles
(shared-memory bandwidth bound), register blocking (stalls while tiles load), and a pipelined kernel with
`cp.async` double buffering, which reaches 82% of cuBLAS on an A100.

**Alternatives.** Use cuBLAS, or start from a fast kernel.

**Why.** Each step shows one cause of lost speed, measured; that is what a kernel compiler automates, and what a
compiler engineer must be able to reason about.

### 12. A CPU checker for GPU kernels

**Decision.** `cpu_check.cc` runs the kernels' own code on the CPU: each CUDA thread a `std::thread`, each
`__syncthreads` a barrier, shared arrays shared by the block.

**Why.** A wrong index on a GPU gives garbage, not a crash, and a GPU job waits in a queue. It cannot check timing:
asynchronous copies finish at once there, so a missing `cp.async` wait shows only on a GPU.

### 13. Triton as the compiler-generated baseline

**Decision.** The same matmul in Triton, benchmarked next to the hand-written kernels, with its IR read at each stage
([IR_WALKTHROUGH.md](../compiler/kernels/triton/IR_WALKTHROUGH.md)).

**Why.** It shows what a tile-level compiler automates (layouts, the pipeline and its waits, swizzling, tensor
cores) and that it does it well: 95% of cuBLAS in FP32, against the hand-written 82%. Triton's design (tile-level
ops, layouts in the IR, lowering a level at a time) is the model for Nova's own tensor path.

## Diagnostics

What the compiler says when a program is wrong. The full list of errors is [ERRORS.md](ERRORS.md).

### D1. One engine, stable codes, and two outputs

**Decision.** Every error and warning goes through one engine (`diagnostics.h`), as a code (`E0502`), a severity, a
source range, a message, notes and fixes. The codes are listed once (`diagnostic_codes.def`, an X-macro) and each
has an entry in ERRORS.md, which the test runner checks. The engine prints clang's form,
`file:line:col: error[E0502]: message`, with the source line and its range underlined, or one JSON document
(`--diagnostics=json`).

**Alternatives.** Messages formatted where they are found (as before: `Error at line N: ...` strings, some on stdout,
some ending the process); or numbered errors without documentation.

**Why.** An editor needs a range, not a line, and a language server needs the errors as data, not text to parse
back. Stable codes make an error searchable and let its explanation change without breaking anything. The
`file:line:col:` form is what editors and VS Code's problem matchers already read.

**Cost.** Every error site names its code, and a new kind of error needs a code and an entry in ERRORS.md.

### D2. Recovery at statements, built on the parser's failure mode

**Decision.** After a syntax error, the parser replaces the statement (or definition, member, case) with an Error
node and goes on at the next one. The scanner reports its errors and goes on too: a stray `{` or `;` is skipped, `!`
reads as `not`, an unclosed string ends at the end of its line, and an unclosed bracket ends at the next line that
can only start a statement. Only the first syntax error of a line is reported, and the names and types are checked
only once the file parses.

**Alternatives.** Exceptions thrown to a statement boundary; error productions in the grammar; checking types on a
tree with holes, as Clang and Swift do.

**Why.** The parser already ended every loop after an error, by reading every token as the end of the file; the
loops over statements now catch that, cut the failed statement's nodes off, and resync. That needs no exceptions
(the MLIR build has none) and no checks inside the recursive descent. Checking a tree with holes means suppressing
the errors the holes cause ("unknown name" for a variable whose declaration did not parse); Go makes the same
choice not to, and the syntax errors are fixed first anyway.

**Cost.** A file with a syntax error shows no type errors until it parses. In an editor, the language server can
keep showing the last type errors it found while a line is being typed.

### D3. Runtime errors at the expression, with the calls

**Decision.** Each instruction keeps the source location of the expression it runs; the interpreter keeps the
instruction it is running, and a call frame keeps its call. A runtime error is a diagnostic (an `R` code) at that
expression, with a "called from here" note for each running call; a recursion is one note with its count.

**Alternatives.** The line only, as before; or a source map built only when an error happens.

**Why.** The line does not say which division divided by zero. The cost is one store per instruction and one per
call, which measures as nothing on the interpreter benchmarks (within the noise of `calls`, `int_loop` and
`vector_sum`).

### D4. Fuzzing the front end

**Decision.** A libFuzzer target runs any bytes through the scanner, the parser, the check pass and both printers,
under ASan and UBSan; CI fuzzes for two minutes per pull request, starting from the golden tests and the standard
library.

**Why.** A language server runs the front end on every keystroke, on text that is mostly wrong. Error recovery is
exactly the code that only odd input reaches.

## Proposed: step C, the `nova` dialect

The plan for Nova IR as an MLIR dialect in SSA form (roadmap step C), for review before it is built.

### C1. Lower from the checked tree, beside today's IR

**Decision.** A second lowering, `lower_mlir.cc`, reads the same check-pass result as `lower.cc` and writes MLIR.
Both back ends stay: the interpreter runs today's IR, and the MLIR path compiles to native code. A test passes only
if both print the same output. Today's IR goes once the MLIR path covers every test.

**Alternatives.** Translate today's IR into MLIR, or replace it at once.

**Why.** Today's IR has already lost the structure MLIR wants (loops are jumps, values are numbered slots
written many times); the checked tree still has it. Running both is the strangler pattern again (decision 6), and
gives differential testing for free: every golden test checks the new compiler against the old one.

### C2. Upstream dialects where they fit, `nova` ops only for what is Nova's own

**Decision.**

| Nova | MLIR | Dialect |
|---|---|---|
| `int`, `float`, `double`, `bool` and their arithmetic | `i32`, `f32`, `f64`, `i1`; `arith.addi`, `arith.mulf`, `arith.cmpi`, ... | `arith` (upstream) |
| `if` / `while` / `for` | `scf.if`, `scf.while`, `scf.for` | `scf` (upstream) |
| functions, calls, several return values | `func.func`, `func.call` (several results built in) | `func` (upstream) |
| a generic function's copies | one `func.func` per copy (`@scale<int, int>`) | `func` |
| arrays | `memref<?xi32>` | `memref` (upstream) |
| `Tensor` and its built-ins | `!nova.tensor<f64>` (rank known at run time); `nova.matmul`, `nova.add`, `nova.relu`, ... | `nova` |
| objects, `vector<T>`, `map<K, V>`, strings | `!nova.object<"Point">`, `!nova.vector<i32>`, `!nova.string`; `nova.new`, `nova.get_field`, `nova.call_method`, ... | `nova` |
| `print`, `input`, `hf.*`, `sol.*` | `nova.print`, ... (calls into the runtime library) | `nova` |

**Alternatives.** A `nova` op for everything (`nova.add`, `nova.if`), lowered to upstream dialects later.

**Why.** `arith`, `scf` and `func` come with their optimizations (constant folding, CSE, LICM, inlining) and their
lowering to LLVM. A `nova.add` would need all of that written again. Custom ops are for semantics no upstream
dialect has: reference-counted objects, virtual calls, Nova's tensors. This is how Triton is built: `tt` ops beside
`arith` and `scf`.

### C3. Tensors are references in `nova`, values only where it is safe

**Decision.** `!nova.tensor` is a reference to a buffer, as a Nova tensor is (`tensor_set` changes it in place, and
two variables can share one). The tensor built-ins lower to `linalg` ops on `memref` buffers (`nova.matmul` to
`linalg.matmul` or `linalg.batch_matmul`, and a broadcast batch to `linalg.generic` with index maps that reuse one
operand), then to loops or GPU code. Ranked types, once the check pass tracks shapes, let these lowerings use
static sizes. A
later pass gives a function's tensors value semantics where nothing mutates or shares them, so that `linalg` on
`tensor` values can fuse them.

**Alternatives.** MLIR's built-in `tensor` type from the start: values, never changed.

**Why.** Value semantics are what fusion and the `linalg` transformations want, but they do not describe a
language whose tensors change in place; modelling them as values would be wrong for `tensor_set`. Starting with
references and recovering values where it is provably safe is what PyTorch does (its "functionalization" pass).

### C4. Reference counting as explicit ops, inserted by a pass

**Decision.** The lowering writes plain `nova` ops; a pass then inserts `nova.retain` / `nova.release` where values
are copied and die, and a second pass removes pairs that cancel. They lower to calls into a small C runtime library
(the object and container heaps, strings).

**Alternatives.** Hide the counting inside each op's lowering, as the interpreter does at run time.

**Why.** Explicit counts are visible to optimization: a retain followed by its release, with nothing between,
disappears. Swift's SIL does the same (automatic reference counting as ops in its IR).

### C5. Native code through LLVM, run in-process or written out

**Decision.** `arith`, `scf`, `func`, `memref` and the lowered `nova` ops go to MLIR's `llvm` dialect, then to LLVM
IR, then to machine code: run at once through MLIR's ExecutionEngine (LLVM's ORC JIT) for `nova --native file.nova`,
or written as an object file and linked with the runtime library. Source locations come from the token locations,
so errors and debug information name Nova lines.

**Why.** It replaces the removed x86 emitter and JIT with LLVM's code generation, on x86 and ARM, and the
interpreter remains for the REPL, the debugger and as the reference.

### C6. Autograd as a transformation on `nova`

**Decision.** Reverse-mode differentiation becomes a pass over a function's tensor ops: it walks them backwards and
writes the backward ops (for `nova.matmul`, the two matmuls of its gradients), so the forward and backward graphs
are lowered and optimized alike.

**Alternatives.** Keep the run-time tape.

**Why.** A tape records operations as they run, so the compiler never sees the backward graph and cannot fuse or
tile it. Generating it at compile time is what JAX does. MLIR has no built-in automatic differentiation; Enzyme
works at the LLVM IR level, below where tensor ops are visible.

### Build and order

- **Build.** The dialect's ops and types are declared in TableGen (ODS) and generated by `mlir-tblgen`, so the MLIR
  part builds with CMake against a pinned LLVM / MLIR release (apt.llvm.org's `libmlir-dev` and `mlir-tools`), on
  Linux and WSL2. A `nova-opt` tool runs the passes on `.mlir` files, as `mlir-opt` does, for tests in MLIR's
  FileCheck style. The interpreter keeps its Makefile until the switch.
- **Order**, each step tested against the interpreter on the golden tests it covers:
  1. the dialect's skeleton, `nova-opt`, and a CI job against the pinned LLVM;
  2. scalars, control flow, functions and `print`, to native code;
  3. arrays, strings and structs;
  4. tensors on CPU: `memref` and `linalg`;
  5. objects and containers, with the reference-counting passes;
  6. autograd as a pass, then tensors on the GPU (`linalg` tiled to the `gpu` dialect and NVVM, or IREE).
- **Risks.** MLIR's C++ API changes between releases (pin one release, upgrade on purpose); CMake and TableGen next
  to the Makefile; and scope, which the order above keeps small: step 2 alone gives a native compiler for a useful
  subset of Nova.

## Still open

- Reference counting leaves cycles; `weak` fields are planned.
- `int` is 32 bits and overflows silently.
- The IR is a slot-numbered instruction list; step C replaces it with an MLIR dialect in SSA form.
