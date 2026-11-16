# Matmul kernels

CUDA kernels for Nova's `matmul` and batched `matmul` (bmm), written by hand to learn what makes a GPU matmul fast
before the tensor compiler work (docs/ROADMAP.md, section 4, step A). Nothing in the compiler calls them yet: they
are the baseline that Triton's kernels, and later Nova's own compiled kernels, are measured against.

| Kernel | What it adds |
|---|---|
| `matmul_naive` | One thread per element of C, every read from global memory |
| `matmul_tiled` | 32 x 32 tiles of A and B in shared memory: each value read from global memory is used 32 times |
| `matmul_blocked` | Register blocking: each thread computes a 4 x 4 piece of a 64 x 64 tile from registers |
| `matmul_pipelined` | 128 x 128 tiles, 8 x 8 per thread, 16-byte loads, and two copies of each tile in shared memory: `cp.async` loads the next step's tiles while the current ones are used |

All four take a batch along the grid's z with a stride per matrix; a stride of 0 uses one matrix for the whole
batch, which is Nova's `(n, k) @ (b, k, m)`. `bench` checks each kernel's result against cuBLAS
(`cublasSgemmStridedBatched`) and then times it.

Without a GPU, `make cpu-check` runs the kernels' own code on the CPU (each CUDA thread a `std::thread`, each
`__syncthreads` a barrier) and checks their results against a plain loop, including odd sizes and a broadcast A.
It catches indexing mistakes before a SOL job does; only `bench` says anything about speed.

## Triton

`triton/matmul.py` is the same matmul in Triton. A Triton kernel is written per tile: a program owns one tile of C,
loads blocks of A and B (masked at the edges) and multiplies them with `tl.dot`; the compiler decides the rest, which
is everything the CUDA kernels above do by hand (threads, shared memory, 16-byte loads, the `cp.async` pipeline from
`num_stages`, and tensor cores for TF32). An autotuner picks the tile sizes per problem size. It runs the same five
cases against cuBLAS in FP32 and TF32, and `--dump-ir DIR` writes one kernel's IR at each stage of Triton's compiler
(Triton IR, Triton GPU IR, LLVM IR, PTX).

```sh
# once, on SOL: a Python with PyTorch, which brings Triton (run_triton.sh says which it looks for)
conda create -n nova-triton python=3.11 -y && conda activate nova-triton && python -m pip install torch

cd triton && sbatch run_triton.sh       # benchmark, then the IR into triton/ir/
PYTHON=/path/to/python sbatch run_triton.sh   # or with a given Python
cat triton-<job id>.out
```

Without a GPU, `TRITON_INTERPRET=1 python matmul.py --check` runs the kernel in Triton's interpreter on the CPU
(it needs NumPy older than 2.3).

## Running on SOL

Either way in, the job runs on a GPU node through Slurm:

```sh
# 1. get this directory onto SOL: ssh sol.asu.edu and git pull there, or copy it
scp -r compiler/kernels <asurite>@sol.asu.edu:~/nova-kernels      # direct
#    (through the EC2 tunnel instead: ssh -p 2223 <asurite>@localhost on EC2, then git pull or scp from there)

# 2. on SOL, from that directory
sbatch run_sol.sh                       # the default cases
sbatch run_sol.sh 2048 2048 2048 8      # one case: M N K [batch] [broadcast]
squeue -u $USER                         # wait for it
cat matmul-<job id>.out
```

`run_sol.sh` asks for one A100 (`--gres=gpu:a100:1`) and builds for `sm_80`; for an H100 use `--gres=gpu:h100:1`
and `ARCH=sm_90 sbatch run_sol.sh`. If its CUDA module is not found, set `CUDA_MODULE` to one listed by
`module avail cuda`.

## Results

Measured on SOL's NVIDIA A100-SXM4-80GB (108 SMs; FP32 peak 19.5 TFLOP/s), CUDA 12, share of cuBLAS's speed. Every
kernel's result matches cuBLAS on every case.

| Case | cuBLAS | naive | tiled | blocked | pipelined |
|---|---|---|---|---|---|
| 4096 x 4096 x 4096 | 19.0 TFLOP/s | 13% | 27% | 50% | 82% |
| 32 x 512³, one A for the batch | 17.7 TFLOP/s | 17% | 29% | 55% | 79% |
| 64 x 256³ (batched) | 14.0 TFLOP/s | 21% | 35% | 55% | 77% |
| 1024 x 1024 x 1024 | 16.6 TFLOP/s | 18% | 30% | 41% | 48% |
| 1000 x 999 x 1001 | 15.6 TFLOP/s | 22% | 30% | 42% | 43% |

What limits each step:
- **naive:** memory. Two loads per multiply-add, each value used once; at 4096 the matrices (192 MB) no longer fit
  in the 40 MB L2 cache.
- **tiled:** shared-memory bandwidth. Two shared reads per multiply-add caps it near 4.9 TFLOP/s, which it reaches
  on every case.
- **blocked:** 0.5 shared reads per multiply-add, but each step loads, waits at a barrier, then computes, so the
  arithmetic units idle while tiles load.
- **pipelined:** 0.25 shared reads per multiply-add and loads overlapped with the arithmetic. On small problems its
  128 x 128 tiles make too few blocks: 1024 x 1024 is 64 blocks for 108 SMs, so 44 SMs idle. Libraries pick the
  tile size per problem (or split K across blocks); Triton's autotuner does the same.

The next steps on this path are a deeper pipeline (3 or 4 stages), larger K steps, warp-level tiling and
16-byte stores; past the FP32 peak, tensor cores (TF32 peaks near 156 TFLOP/s on an A100).

### Triton

The same A100, torch 2.6 with Triton 3.2; share of cuBLAS's speed, FP32 against cuBLAS's FP32 and TF32 against
its TF32, and the configuration the autotuner picked. Every result matches cuBLAS (TF32's error is about 7e-4,
from rounding each input to a 10-bit mantissa; FP32's about 1e-6).

| Case | pipelined (CUDA) | Triton FP32 | Triton TF32 | Triton TF32 speed | Tiles picked (FP32 / TF32) |
|---|---|---|---|---|---|
| 4096 x 4096 x 4096 | 82% | 95% | 84% | 111 TFLOP/s | 64x64x32, 4 stages / 128x256x32, 3 stages |
| 32 x 512³, one A for the batch | 79% | 99% | 89% | 83 TFLOP/s | 64x64x32 / 128x64x32 |
| 64 x 256³ (batched) | 77% | 112% | 98% | 42 TFLOP/s | 64x64x32 / 128x64x32 |
| 1024 x 1024 x 1024 | 48% | 82% | 90% | 56 TFLOP/s | 64x64x32 / 64x64x32 |
| 1000 x 999 x 1001 | 43% | 67% | 100% | 30 TFLOP/s | 64x64x32 / 128x128x16 |

- For FP32 the autotuner picked 64 x 64 x 32 tiles with a 4-stage pipeline on every case, where the pipelined
  kernel uses 128 x 128 x 8 with 2: a deeper pipeline and a larger K step (4 times fewer barriers) beat bigger
  tiles, and 64 x 64 tiles make enough blocks for small problems (1024 x 1024 goes from 48% to 82%).
- 1000 x 999 x 1001 is slower in FP32 because Triton specializes a kernel on whether its sizes and pointers are
  divisible by 16; these are not, so it cannot prove 16-byte loads aligned, the choice the pipelined kernel's
  `VEC` makes by hand.
- On 64 small products Triton beats cuBLAS: cuBLAS chooses its batched kernel by heuristics, the autotuner by
  measuring.
- TF32 runs on tensor cores: cuBLAS reaches 132 TFLOP/s on 4096 x 4096 (7 times its FP32 rate, 85% of the TF32
  peak), and the autotuner picks larger tiles to keep the tensor cores fed.
