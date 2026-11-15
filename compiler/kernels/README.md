# Matmul kernels

CUDA kernels for Nova's `matmul` and batched `matmul` (bmm), written by hand to learn what makes a GPU matmul fast
before the tensor compiler work (docs/ROADMAP.md). Nothing in the compiler calls them yet: after phase 7, Nova IR's
tensor ops either call kernels like these or are compiled by IREE, and this benchmark is what either is measured
against.

| Kernel | What it adds |
|---|---|
| `matmul_naive` | One thread per element of C, every read from global memory |
| `matmul_tiled` | 32 x 32 tiles of A and B in shared memory: each value read from global memory is used 32 times |
| `matmul_blocked` | Register blocking: each thread computes a 4 x 4 piece of a 64 x 64 tile from registers |

All three take a batch along the grid's z with a stride per matrix; a stride of 0 uses one matrix for the whole
batch, which is Nova's `(n, k) @ (b, k, m)`. `bench` checks each kernel's result against cuBLAS
(`cublasSgemmStridedBatched`) and then times it.

Without a GPU, `make cpu-check` runs the kernels' own code on the CPU (each CUDA thread a `std::thread`, each
`__syncthreads` a barrier) and checks their results against a plain loop, including odd sizes and a broadcast A.
It catches indexing mistakes before a SOL job does; only `bench` says anything about speed.

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

## What to expect

On an A100, cuBLAS reaches most of the FP32 peak (about 19.5 TFLOP/s) on 4096 x 4096. The naive kernel is far
below it, the tiled one a few times faster, and the register-blocked one a good share of cuBLAS. The next steps
along the same path are vectorised `float4` loads, double-buffered shared memory, warp tiling, and tensor cores
(TF32 / FP16 through WMMA), which is where cuBLAS gets the rest.
