# Triton's IR, stage by stage

What Triton's compiler does to `matmul.py`, read from the IR it wrote at each stage (`python matmul.py --dump-ir ir`
on SOL's A100, Triton 3.2, one configuration: 128 x 128 x 32 tiles, 8 warps, 3 stages), and where each step meets
what the CUDA kernels in `../matmul_kernels.cuh` do by hand.

| Stage | File | Lines (FP32 / TF32) | What it adds |
|---|---|---|---|
| 1. Triton IR | `.ttir` | 167 / 167 | The Python kernel as MLIR, tile by tile: what is computed, not how |
| 2. Triton GPU IR | `.ttgir` | 245 / 269 | Layouts (which thread holds which element), shared memory, the software pipeline |
| 3. LLVM IR | `.llir` | 1,745 / 1,579 | One thread's view: its share of every tile as registers and vectors, address spaces, barriers |
| 4. PTX | `.ptx` | 3,881 / 1,679 | NVIDIA's virtual instruction set: `cp.async`, `fma` or `mma.sync`, `bar.sync` |

## 1. Triton IR: tiles, in SSA form

```mlir
tt.func public @matmul_kernel(%arg0: !tt.ptr<f32> {tt.divisibility = 16}, ..., %arg3: i32 {tt.divisibility = 16}, ...)
  %0 = tt.get_program_id x : i32                                             // pid
  %16 = tt.make_range {start = 0, end = 128} : tensor<128xi32>               // tl.arange(0, BLOCK_M)
  %25 = tt.expand_dims %18 {axis = 1} : tensor<128xi32> -> tensor<128x1xi32> // rows[:, None]
  %33 = tt.addptr %31, %32 : tensor<128x32x!tt.ptr<f32>>, ...                // a_ptrs: a block of pointers
  %46 = arith.cmpi slt, %25, %45 : tensor<128x1xi32>                         // rows < M, before the loop
  %53:3 = scf.for %k = %c0 to %K step %c32 iter_args(%acc = %zeros, %a_ptrs = %33, %b_ptrs = %44) -> (...) {
    %75 = tt.load %a_ptrs, %mask, %zeros : tensor<128x32x!tt.ptr<f32>>
    %80 = tt.load %b_ptrs, %mask2, %zeros : tensor<32x128x!tt.ptr<f32>>
    %81 = tt.dot %75, %80, %acc : tensor<128x32xf32> * tensor<32x128xf32> -> tensor<128x128xf32>
    scf.yield %81, %82, %83                                                  // the next iteration's acc, a_ptrs, b_ptrs
  }
  tt.store %62, %53#0, %69 : tensor<128x128x!tt.ptr<f32>>
```

- **SSA.** Every value is defined once. What Python reassigns in the loop (`acc`, `a_ptrs`, `b_ptrs`) is carried
  as `iter_args`, and `scf.yield` hands each iteration's values to the next: MLIR's form of the phi node.
- **Dialects.** Triton's own (`tt.load`, `tt.dot`, `tt.addptr`) next to MLIR's `arith` and `scf`. Lowering replaces
  ops of one dialect with ops of a lower one, a level at a time.
- **Specialization.** 12 arguments, not 15: the three strides equal to 1 at launch are compiled in as constants.
  `tt.divisibility = 16` records that every size and pointer is a multiple of 16, which stage 2 uses to prove
  16-byte loads aligned (1000 x 999 x 1001 lacks it, and runs slower). The CUDA kernel's `VEC` makes the same choice
  by hand.
- **Loop-invariant code motion.** `rows < M` and `cols < N` are computed before the loop; only the K mask stays
  in it. On SSA values, which never change, the safety rules Nova's LICM needs (liveness, no read before the write)
  are not needed.
- **Constants.** `BLOCK_M`, `BLOCK_K` and `GROUP_M` are literals and every shape is fixed: each `tl.constexpr`
  combination is its own kernel, like a C++ template instantiation.

Nothing here says which thread does what: that is stage 2.

## 2. Triton GPU IR: layouts and the pipeline

### Layouts

```mlir
#blocked  = #triton_gpu.blocked<{sizePerThread = [4, 4], threadsPerWarp = [1, 32], warpsPerCTA = [8, 1], order = [1, 0]}>
#blocked2 = #triton_gpu.blocked<{sizePerThread = [1, 4], threadsPerWarp = [4, 8],  warpsPerCTA = [8, 1], order = [1, 0]}>
#blocked1 = #triton_gpu.blocked<{sizePerThread = [1, 4], threadsPerWarp = [1, 32], warpsPerCTA = [8, 1], order = [1, 0]}>
#shared   = #triton_gpu.shared<{vec = 1, perPhase = 1, maxPhase = 1, order = [1, 0]}>
```

A layout is a function from a tile's coordinates to (warp, thread, register), and every tensor type now carries
one (`tensor<128x128xf32, #blocked>`).

| Layout | For | Says | By hand |
|---|---|---|---|
| `#blocked` | the 128 x 128 accumulator | 4 x 4 per thread, 32 threads across, 8 warps down; the 32 x 128 pattern repeats 4 times, so each thread holds 64 values in 4 groups of rows 32 apart | the pipelined kernel's 8 x 8 per thread, in 2 groups of rows 64 apart |
| `#blocked2` | loading A's 128 x 32 block | 4 floats per thread (16-byte loads), 8 threads per 32-float row | `cp_async_16` |
| `#blocked1` | loading B's 32 x 128 block | 4 floats per thread, 32 threads along a row: a warp reads 512 contiguous bytes | coalesced reads of B |
| `#shared` | the tiles in shared memory | no swizzling (FP32) | `PAD = 4` |

Where two ops want different layouts, the compiler inserts `triton_gpu.convert_layout`: before the store, the
accumulator moves from `#blocked` to `#blocked1` through shared memory, so C is written in wide, coalesced stores.

### The pipeline (`num_stages = 3`)

```mlir
%59 = triton_gpu.local_alloc : () -> !tt.memdesc<2x128x32xf32, #shared, ...>     // 2 buffers for A
%69 = triton_gpu.async_copy_global_to_local %38, %66 mask %68 other %cst_1       // steps 0 and 1 start before the loop
%70 = triton_gpu.async_commit_group %69
%102 = triton_gpu.async_wait %79 {num = 2 : i32}
scf.for ... iter_args(%acc, %a_ptrs, %b_ptrs, %insert_idx, %extract_idx, %a_buf, %b_buf, %token) {
  %122 = triton_gpu.local_load %a_buf -> tensor<128x32xf32, #triton_gpu.dot_op<{opIdx = 0, parent = #blocked}>>
  %124 = tt.dot %122, %123, %acc
  %139 = triton_gpu.async_copy_global_to_local ...                               // step k + 2 starts
  %154 = triton_gpu.async_wait %token {num = 2 : i32}
}
```

| The pipelined CUDA kernel | Triton GPU IR |
|---|---|
| `__shared__ float As[2][BM][BK + PAD]` | `local_alloc : memdesc<2x128x32xf32, #shared>` |
| `load(0, 0)` before the loop | a prologue that starts steps 0 and 1 |
| `load(cur ^ 1, ...)` | `memdesc_subview %59[%insert_idx]`, the buffer index carried by the loop |
| `cp_async_commit()` / `cp_async_wait<1>()` | `async_commit_group` / `async_wait {num = 2}` |
| the two `__syncthreads()` | not in this IR: a later pass (`Membar`) finds the shared-memory hazards and inserts barriers (12 in the LLVM IR and the PTX) |

Three stages with two buffers: the third is in registers. Each iteration first copies the current tile out of
shared memory (`local_load` into a `dot_op` layout), then starts the next copy into the buffer it emptied, so three
steps are in flight at once. The pipelined kernel has two. And the wait that kernel needed by hand (missing it
gave wrong results the CPU checker could not see) is inserted here by analysis.

### TF32: tensor cores

```mlir
#mma    = #triton_gpu.nvidia_mma<{versionMajor = 2, warpsPerCTA = [4, 2], instrShape = [16, 8]}>
#shared = #triton_gpu.shared<{vec = 4, perPhase = 1, maxPhase = 8, order = [1, 0]}>
```

- `#mma` (version 2: Ampere's `mma.sync`) is the accumulator layout the tensor-core instruction requires: each
  instruction computes a 16 x 8 piece, and the 8 warps sit 4 x 2 over the tile. The hardware dictates it, and the
  compiler fits the rest around it.
- The shared tiles are swizzled: the columns of each row are permuted by XOR, so the reads `mma` needs never hit
  one bank twice. Padding (the CUDA kernel's choice) wastes a little memory; swizzling wastes none but costs
  address arithmetic, which a compiler does without effort.

## 3. LLVM IR: one thread's view

```llvm
define void @matmul_kernel(ptr addrspace(1) %0, ptr addrspace(1) %1, ptr addrspace(1) %2, i32 %3, ...) {
!5 = !{ptr @matmul_kernel, !"reqntidx", i32 256}                       ; exactly 256 threads per block
  %197 = phi ptr addrspace(3) [ getelementptr (i8, ptr addrspace(3) @global_smem, i64 32768), %.lr.ph ], [ %1245, %196 ]
  tail call void asm sideeffect "@$3 cp.async.cg.shared.global [ $0 + 0 ], [ $1 + 0 ], 0x10, $2;", "r,l,r,b"(...)
  %586 = tail call <64 x float> @llvm.fmuladd.v64f32(<64 x float> %585, <64 x float> %493, <64 x float> %202)
  call void @llvm.nvvm.barrier0()                                        ; __syncthreads, inserted by Membar
```

- **The tile is gone; each thread's share remains.** The 128 x 128 accumulator is now a `<64 x float>` vector per
  thread (`#blocked`'s 64 values), and each of the 32 k's of a step is one `fmuladd` on all 64: 32 calls, the step
  unrolled. This is the step the layout made possible: it says exactly which 64 values a thread owns.
- **Address spaces.** `addrspace(1)` is global memory, `addrspace(3)` shared memory. Shared memory is one block,
  `@global_smem`, and B's buffers start 32,768 bytes in: 2 x 128 x 32 floats for A, the same for B, 64 KB in all.
  That is more than the 48 KB a kernel may declare statically, so it is allocated at launch (`.extern .shared` in
  the PTX). The pipelined CUDA kernel uses 20 KB.
- **Phi nodes.** The loop is now basic blocks, and the `iter_args` of stage 1 are `phi` nodes (15 of them): the
  same SSA idea in LLVM's spelling.
- **Inline PTX.** `cp.async` has no LLVM intrinsic in this form, so Triton emits it as inline assembly, as the
  pipelined CUDA kernel does with `asm volatile`.

## 4. PTX

```ptx
.extern .shared .align 16 .b8 global_smem[];
.visible .entry matmul_kernel( ... )
.reqntid 256, 1, 1
    @%p57 cp.async.cg.shared.global [ %r170 + 0 ], [ %rd33 + 0 ], 0x10, %r171;
    cp.async.commit_group ;
    ld.shared.f32  %f193, [%r361+12704];
    fma.rn.f32 ...                                                     ; 2,048 in the FP32 file
    mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32 { %f642, ... }, { %r1214, ... }, { %r1209, %r1207 }, { %f642, ... };
    @%p73 st.global.v4.b32 [ %rd98 + 0 ], { %r467, %r468, %r469, %r470 };
```

| | FP32 | TF32 |
|---|---|---|
| Lines | 3,881 | 1,679 |
| The arithmetic | 2,048 `fma.rn.f32`: 64 accumulators x 32 k's, the step fully unrolled | 64 `mma.sync.m16n8k8`, each a 16 x 8 x 8 product on tensor cores |
| Loads | `cp.async.cg.shared.global ..., 0x10, %r`: the pipelined kernel's own instruction, 16 bytes with zero-fill | the same |
| Stores | `st.global.v4.b32`: 16-byte stores, from the `convert_layout` before the store | the same |

PTX registers are virtual and unlimited (`.reg .f32 %f<2881>`); `ptxas`, NVIDIA's assembler, allocates the real
ones (at most 255 a thread) when it turns PTX into machine code (SASS).

## Questions

1. Why can `acc` not be updated in place in stage 1, and what replaces that?
2. What does Triton gain by compiling a separate kernel when a stride is 1, or a size divisible by 16?
3. What does a layout map from and to, and why must it be computable?
4. Why are there two shared buffers for three stages?
5. Who inserts the barriers, and what bug does that prevent?
6. Padding or swizzling: what does each cost?
7. Why does TF32 dictate the accumulator's layout?
8. Why is the shared memory allocated at launch rather than declared?
