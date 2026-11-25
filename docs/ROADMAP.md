# Roadmap

Where NovaComp is heading. Done so far: the syntax redesign (steps 1–9, stages A and B; see
[SYNTAX.md](SYNTAX.md)), a production front end (phases 1–2) and batched matmul. Section 4 is the plan from here,
in order: each step builds on the ones before it.

## 0. Fix first

These issues turned up in the like-for-like benchmarks. Each has a minimal reproduction in
[BENCHMARKS.md](BENCHMARKS.md#issues-found), and each fix should land with a golden test (the first one,
`--optimize` never finishing on a loop, is fixed):

- `int` is 32-bit and overflows silently.
- Memoization treats a function as pure without checking the functions it calls, so their side effects are
  skipped.
- Freed `tensor_heap` entries are never reused, so long training runs outgrow the 256 MB worker limit
  (generational handles).
- Parts of SYNTAX.md's first example are not implemented yet (`10_000`, `t.grad`, `print(t)`, string
  escapes).

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
  - (fixed) `--optimize` hung while compiling a loop nested in another whose body used the outer loop's variable
    times a constant: loop-invariant code motion linked the instruction list into a cycle (see bmm below).
  - `--dump-ir` prints `strMem[mem[slot]]` for strings that only exist at run time, so on some programs it
    prints garbage or stops partway (the dump only: the programs run correctly).

## 2. A production compiler front end (phases 1–2, done)

The front end was rebuilt the way production compilers build theirs (Clang, rustc, Go, Zig, Carbon, CPython,
Lua). The x86 assembly emitter, the experimental x86 JIT and the `.nbc` bytecode export (with its standalone VM
and the IDE's export) were removed first: they could not keep up with the language, and native code will come
from LLVM (section 4, step C).

**The front end**

1. **Lexer** (done). A pointer-based scanner over one source buffer writes compact tokens (a kind, a 32-bit
   location in one space over all buffers, a length, an interned symbol); names and keywords share one
   interned table (a keyword is recognised by the same lookup); NEWLINE / INDENT / DEDENT are made while
   scanning. `#@line` markers from imports become line directives in the SourceManager, which works out lines
   and columns only when asked. (Until 2c-6, a small compatibility pass rebuilt the braces-and-semicolons form
   for the old parser; the parse tree reads the tokens directly.)
2. **Parse tree and check pass** (done). The parser builds a flat tree (Carbon / Zig style) and a separate pass
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
     (undeclared, redeclared, unknown types, break / return / self out of place, the entry point). 2b-2: types, in
     two steps. 2b-2a (done): one interned table of types, the type of every expression, and the rules for numbers,
     strings, tensors (against each built-in's signature), arrays, structs and function calls, including multiple
     returns and return types inferred from the first `return`. A function's body is checked when a caller first
     needs its return type, so it can be defined after the caller (once 2c removes the old parser). A value not
     typed yet (a field, a method's result, a container element, an untyped parameter) is Unknown and fits
     everywhere. 2b-2b (done): objects and containers. Fields are typed through the parent chain; constructors are
     checked against `init`, method calls by name, argument types and count (plain-type arguments too, which the
     old parser let through), and an override must keep its parent's signature. A subclass fits where its parent
     does and `none` wherever an object does; objects compare with `==` / `!=` only. Vector and map keys, elements
     and methods are checked, with value methods (`v.size()`) told apart from statement methods (`v.push(x)`). A
     method without `-> types` still returns Unknown. 2b-3 (done): generic functions and classes, checked once per
     copy. A call to a generic function makes the copy for its argument types (`scale<int, int>`) and checks its
     body with them; writing `Box<int>` makes Box's copy for `int` and checks its fields and methods with `T` an
     `int`. Copies are distinct types (`Box<int>` is not a `Box<double>`), and messages name them. Unknown is now
     left only for a method's untyped parameter and the result of a method without `-> type`.
   - 2c: lowering from the checked tree to the IR; the old parser and the braces-form pass are removed. In six
     steps: 2c-1 (done): numbers, bools,
     strings, arrays, control flow and functions, lowered by `lower.cc` into the same instructions and slots the old
     parser writes, so the passes after it do not change. Every name and type comes from the check pass; lowering
     keeps no tables by name. A function can call one defined after it, and two functions can call each other. The
     driver lowers a program when lowering covers all of it and hands the rest to the old parser;
     `NOVA_LOWER_ONLY=1` stops instead, to show what is covered. The rule that the ignite block comes last moved to
     the check pass. 2c-2a (done): tensors, every tensor built-in and autograd (they are ordinary tensor calls),
     with each tensor's slot listed in `g_tensorSlots` so lowered programs free their tensors as the old parser's
     do; output and tensor-heap counts match the old parser on the test suite. It also fixed three places that took
     a float or double slot's number as a `mem` slot (a call's result in liveness and in the tensor-free pass, and a
     parameter's retain in the interpreter), which freed a live tensor early or kept a dead one; see
     `tensor_free_number_calls`. 2c-2b (done): `hf` / `sol`, with name=value arguments (`max_tokens=50`,
     `sol.finetune(model=..., task=...)`), which the new parser had stopped accepting; the check pass allows them in
     those calls only and checks every `hf` / `sol` call's arguments. STRCAT now numbers its string from `strMem`
     itself, since an `hf` call adds strings there too. 2c-3 (done): structs and classes. A struct variable is one
     slot per field (a nested struct's fields in its place), and `q = p` copies it field by field, which the old
     parser cannot do. Objects, fields, inheritance and `self` lower to the object instructions; a method call is
     virtual and resolved at run time through `virtualTable`; a temporary object (a new one, a call's result, a field
     read) is moved into a variable or released, so object counts match the old parser's. Methods are registered in
     `classMethodTable`, or the tensor-free pass never sees their bodies and their tensors are not freed. Not covered
     yet, so still handed to the old parser: struct parameters and return values, classes with `vector` / `map`
     fields (2c-4) and generic classes (2c-5). The old parser reads `return 0.0` in a method returning double as a
     double from the float literal's slot, so it prints the wrong value; lowering gets it right, and the bug goes
     with the old parser. 2c-4 (done): `vector` / `map`. Containers are reference-counted as objects are, through the
     same `OBJ_STORE` / `OBJ_MOVE` / `OBJ_RELEASE` (the runtime tells the heaps apart): `vector<int> v` and
     `vector<int>()` make a new, empty one, a container field gets one when its object is made, and a container read
     from a field or an element is a temporary. Elements, methods (`push`, `pop`, `has`, `min`, `sum`, ...) and
     `v[i] = x` / `m[k] = x` lower to the container ops, the `*_D` ones for float and double elements; an object
     pushed or set is counted by the container, and one popped with nothing to take it is dropped. Object and
     container counts match the old parser's. Not covered yet: container values in a multiple declaration, container
     comparisons, container fields of structs, and generic classes and functions (2c-5). 2c-5 (done): generics. Each
     copy the check pass makes keeps its own node types, variable types and the copies its calls run (`nodeTypes`
     alone held only the last copy checked's), and lowering lowers each copy with them: `scale<int, int>` is a function
     of its own and `Box<int>` a class of its own, with its own methods and class id, named as the old parser names
     them. A copy never made is never lowered. Lowering now covers every run test in the suite. Not covered yet: a
     class whose parent is a generic class (`class C extends Box`, which the check pass lets through), struct
     parameters and return values, container values in a multiple declaration, container comparisons, and container
     fields of structs. 2c-6 (done): the old parser (ten files, about 5,800 lines), the braces-form pass and the
     fallback are deleted; every program goes through the parse tree, the check pass and lowering, and `--check`
     stops after the check pass. The gaps above are closed: container `==` / `!=` (by identity) and objects and
     containers in a multiple declaration are lowered, and the rest, which the old parser could not compile either,
     are check-pass errors: struct parameters and return values, struct comparisons, array / object / container
     fields of a struct, a struct field of a class, a parent class that is generic, a switch on anything but an int,
     `string(x)` / `bool(x)`, and several values from a method call. `unlowered()` remains as a last check, so a
     program the check pass lets through by mistake stops with "cannot lower this yet" instead of being lowered
     wrongly. `x += e` works its target's parts out once (`v[f()] += 1` calls `f` once; a temporary part is dropped
     once). The runtime now hands every object or container a call returns back to the caller (it handed back only
     the first, so a second returned container was freed). The REPL lowers each entry as a program of its own: the
     definitions so far, then an ignite block that declares again the variables earlier entries made, which lowering
     binds to the slots they already have (`ReplVariable`); class and method ids are kept by name across entries, so
     earlier objects still dispatch. The debugger needed no change. The old parser's dead state (`own_kind`, the
     field / self slot sets, `classTable` and the other name-keyed tables) is removed, and the tensor built-in tables
     move to `builtins.cc`.
The rest of the original phases (3: error recovery; 4–5: reference-counted strings and arrays, `weak` fields;
6–7: frames on a value stack and a typed Nova IR) are folded into the plan below: 6 and 7 become the MLIR
dialect (step C), and 3–5 move to the end (step F).

## 3. The ML work so far

**Batched matmul (`bmm`)** (done). A right side of rank 3 or more makes `matmul` batched: `(b..., n, k) @
(b..., k, m)` gives `(b..., n, m)`, one matrix product per batch index, forward and backward. The batch
dimensions broadcast as NumPy's do: lined up from the right, a missing dimension or a 1 stretches to the other
side's, so `(n, k) @ (b, k, m)` multiplies the one matrix with each of B's, and a matrix used by several products
gets the sum of their gradients (`tensor_bmm_broadcast`, `grad_bmm_broadcast`). Other batch dimensions must match
(`error_bmm_batch_mismatch`), and a batched matmul needs a left side of rank 2 or more
(`error_bmm_rank_mismatch`). Both kinds of matmul are one loop over matrix products (`matmul_plan`, which records
the matrices of A and B each product takes): a 2-D right side is one product, so 2-D programs compute exactly
what they did. `grad_bmm` checks every gradient element against finite differences. `tensor_bmm` was also the
first program to make loop-invariant code motion move anything, and the move linked the instruction list into a
cycle (`--optimize` never finished). LICM is rewritten: it moves an int `+` / `-` / `*` whose operands the loop
never writes, and only when the moved value is not read before it is written, not read after the loop, and the
loop is entered only through its header (`licm_loops`).

## 4. The plan from here

The aim from here is the compiler work that ML hardware and ML compilers are built on: SSA IRs in MLIR, lowering
through several levels of abstraction, code generation for a hardware target, and optimizations and scheduling
driven by that hardware's memory hierarchy. Each step comes with its design decisions written down
([DESIGN.md](DESIGN.md)): what was chosen, what was not, and why. In order:

**A. GPU kernels by hand, then in Triton.** `compiler/kernels/` holds matmul and batched-matmul kernels in CUDA,
in the classic steps (done): one thread per element, shared-memory tiles, register blocking, and a pipelined
kernel (128 x 128 tiles, `cp.async` double buffering) that reaches 82% of cuBLAS on a 4096³ matmul on SOL's A100s
([results](../compiler/kernels/README.md#results)). A benchmark checks each against cuBLAS and times it, and a CPU
checker runs the kernels' own code without a GPU. The same matmul in Triton (done) reaches 95% of cuBLAS in FP32
and 84–100% in TF32 on tensor cores, its autotuner finding the deeper pipeline and smaller tiles the pipelined
kernel lacks. Its IR is read at each stage (Triton IR, Triton GPU IR, LLVM IR, PTX)
in [IR_WALKTHROUGH.md](../compiler/kernels/triton/IR_WALKTHROUGH.md): layouts (`#blocked`, `#mma`, `#shared`),
coalescing, the software pipeline from `num_stages`, and the mapping to tensor cores. Later kernels on the same path: warp tiling, tensor cores by hand
(WMMA / MMA), and FlashAttention.

**B. The design record** (done). [DESIGN.md](DESIGN.md) records the decisions made so far, each with its
alternatives and trade-offs: the flat postorder parse tree, a check pass separate from lowering, lowering into the
existing IR before replacing it, generics compiled once per copy, reference counting for objects and containers,
the LICM safety rules, broadcasting through `MatmulPlan`, and the kernel steps. Every later step adds its own.

**C. Nova IR as an MLIR dialect, in SSA form** (replaces phases 6 and 7).
- A `nova` dialect (ODS / TableGen) for Nova's operations and types: numbers, tensors with their element type and
  rank, objects and containers, calls, and the tensor built-ins. The check pass's tree is lowered into it in SSA
  form, so every value is defined once and the optimizations become standard ones over SSA (constant
  propagation, CSE, LICM on dominators) instead of the slot-numbered linked list's.
- Progressive lowering, one level at a time:
  - scalar code: `nova` → `arith` / `scf` / `func` / `memref` → `llvm` → LLVM IR, compiled ahead of time or by
    LLVM's JIT, for native code on x86 and ARM;
  - tensors: `nova` → `linalg` (`nova.matmul` becomes `linalg.batch_matmul`, with broadcasting made explicit in
    the lowering) → tiling and fusion → GPU code (`gpu` / NVVM), or IREE as a ready-made path for comparison.
- Autograd as a transformation on the `nova` dialect: the backward graph is built from the forward tensor ops,
  so both are lowered and optimized alike (MLIR has no built-in autodiff).
- The interpreter stays as the reference: every golden test runs through it and through the compiled code, and
  the two must agree (differential testing).
- Progress: the dialect's first slice is in `compiler/mlir/`: `!nova.tensor`, `nova.zeros`, `nova.matmul` (with a
  verifier) and `nova.print`, defined in ODS; a `nova-opt` driver; and lit/FileCheck tests (`check-nova`) for printing
  and parsing, CSE and canonicalization, and the verifier's errors. `nova-lower` lowers the checked tree into it
  (DESIGN.md C1): code in the ignite block (numbers, variables, arithmetic, comparisons, print), built directly in
  SSA form, with if / elif / else and short-circuit and / or as `scf.if` (its results join the branches' values, as
  a phi does), and while / do-while / for as `scf.while` (its block arguments carry the loop's variables), with
  `break` and `continue` as flags the loop carries and tests. Functions are `func.func` and calls `func.call`
  (several results too), with `return` as a flag and result slots the same way. Tensors are `!nova.tensor<f64>`:
  `zeros` / `ones`, `@` / `matmul`, `print`, and element reads and writes (`t[i, j]`, `+=`, `tensor_get` /
  `tensor_set`) lower to the nova dialect. A write is a memory effect, not a new SSA value, and the ops declare
  MemAlloc / MemRead / MemWrite instead of `Pure`, so CSE, LICM and canonicalize treat tensors as references. A
  generic function is lowered once per copy the check pass made, each copy a `func.func` named with its argument
  types (`@"scale<int, int>"`). `+ - * /` with tensors are `nova.add` / `sub` / `mul` / `div`, `nova.scale` and
  `nova.add_scalar`, in the interpreter's forms (`t / k` is `t * (1 / k)`), so both round alike. Next: lowering
  nova, scf and func to LLVM.
- Frames, variable lifetimes and memory (phase 6) come from the dialect's lowering instead of slot ranges.

**D. A tile-level kernel language in Nova.** `kernel` functions that work on blocks of a tensor, as Triton's do:
a program id, block pointers, masked loads and stores, and a block-level `dot`. They are checked by the check pass
and lowered to Triton's own dialect (Triton IR), then compiled and run by Triton on SOL's GPUs, next to step A's
kernels.

**E. A Triton backend for a simulated accelerator.**
- A small accelerator defined for the project: a scratchpad memory, a DMA engine between it and main memory, a
  matrix unit and a vector unit, with an instruction set and a cycle-approximate simulator that reports time and
  how busy each unit was.
- An out-of-tree Triton backend for it: Triton GPU IR is lowered to an accelerator dialect and then to its
  instructions, so Triton kernels (step A's and step D's) run on it unchanged.
- Hardware-aware passes, each measured on the simulator: tile sizes chosen to fit the scratchpad, double-buffered
  DMA overlapped with compute, data layouts chosen for the matrix unit, and instruction scheduling across the
  units.

**F. Later, as the steps above need them.** Error recovery (one error per mistake, with columns); strings and
arrays as reference-counted heap values and `weak` fields for cycles; and the fixes listed in section 0.

## What stays our own

The lexer, parser, type system, generics, the `nova` dialect and its lowering, the tensor / autograd design, the
kernel language and the accelerator backend are NovaComp's own work. Register allocation, instruction selection
and native code generation come from LLVM; the GPU code for Triton kernels from Triton.

## Costs to plan for

LLVM and MLIR are large C++ dependencies (multi-GB builds, APIs that change between releases): use prebuilt
packages (apt.llvm.org's `libmlir-dev` and `mlir-tools`) on Linux or WSL2, and pin one release in CI. Triton
pins its own LLVM, so step E builds Triton from source once against it. Windows builds are harder; Linux (CI,
WSL2, SOL) is the main target.

## Platform follow-ups

- A refresh flow for the 1-hour session token (users sign in again every hour today).
- The deploy workflow's EC2 host and user secrets, and HTTPS-only `NOVA_ALLOWED_ORIGINS` on the host, before
  the first automatic deploy.
