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

## Still open

- Reference counting leaves cycles; `weak` fields are planned.
- `int` is 32 bits and overflows silently.
- The IR is a slot-numbered instruction list; step C replaces it with an MLIR dialect in SSA form.
