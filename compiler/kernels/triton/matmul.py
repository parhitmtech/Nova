"""The matmul of ../matmul_kernels.cuh written in Triton, benchmarked against cuBLAS, with its IR dumped.

Triton kernels are written per tile: a program instance owns one BLOCK_M x BLOCK_N tile of C, loads blocks of A and
B, and multiplies them with tl.dot. Below the tile the compiler decides everything the CUDA kernels did by hand:
which thread computes what, shared memory and its layout, coalesced and 16-byte loads, the cp.async pipeline
(num_stages), and tensor-core instructions (tl.dot with input_precision="tf32").

    python matmul.py                   benchmark: cuBLAS and Triton, FP32 and TF32, on bench.cu's five cases
    python matmul.py --dump-ir DIR     write one kernel's IR at each stage (ttir, ttgir, llir, ptx) to DIR
    python matmul.py --check           check the kernel's results on small cases (TRITON_INTERPRET=1 runs it on the CPU)
"""
import argparse
import os
import sys

import torch
import triton
import triton.language as tl


@triton.jit
def matmul_kernel(a_ptr, b_ptr, c_ptr, M, N, K,
                  stride_ab, stride_am, stride_ak,
                  stride_bb, stride_bk, stride_bn,
                  stride_cb, stride_cm, stride_cn,
                  BLOCK_M: tl.constexpr, BLOCK_N: tl.constexpr, BLOCK_K: tl.constexpr, GROUP_M: tl.constexpr,
                  TF32: tl.constexpr):
    # One program per tile of C; the batch is the grid's second axis, as blockIdx.z is in the CUDA kernels, and a
    # batch stride of 0 uses one matrix for the whole batch.
    pid = tl.program_id(0)
    batch = tl.program_id(1)

    # Tiles in groups of GROUP_M rows: programs that run at the same time share rows of A and columns of B, so more
    # of what they load is still in the L2 cache.
    tiles_m = tl.cdiv(M, BLOCK_M)
    tiles_n = tl.cdiv(N, BLOCK_N)
    per_group = GROUP_M * tiles_n
    first_m = (pid // per_group) * GROUP_M
    group_m = min(tiles_m - first_m, GROUP_M)
    pid_m = first_m + (pid % per_group) % group_m
    pid_n = (pid % per_group) // group_m

    # The tile's rows, columns and the first step's k, as vectors: a block of pointers is built from them
    rows = pid_m * BLOCK_M + tl.arange(0, BLOCK_M)
    cols = pid_n * BLOCK_N + tl.arange(0, BLOCK_N)
    ks = tl.arange(0, BLOCK_K)
    a_ptrs = a_ptr + batch * stride_ab + rows[:, None] * stride_am + ks[None, :] * stride_ak
    b_ptrs = b_ptr + batch * stride_bb + ks[:, None] * stride_bk + cols[None, :] * stride_bn

    acc = tl.zeros((BLOCK_M, BLOCK_N), dtype=tl.float32)
    for k0 in range(0, K, BLOCK_K):
        k_left = K - k0
        # Masked loads: elements past a matrix's edge load as 0, as the CUDA kernels' zero-fill does
        a = tl.load(a_ptrs, mask=(rows[:, None] < M) & (ks[None, :] < k_left), other=0.0)
        b = tl.load(b_ptrs, mask=(ks[:, None] < k_left) & (cols[None, :] < N), other=0.0)
        if TF32:
            acc = tl.dot(a, b, acc, input_precision="tf32")  # tensor cores, inputs rounded to 10-bit mantissas
        else:
            acc = tl.dot(a, b, acc, input_precision="ieee")  # exact FP32 multiply-adds, as the CUDA kernels do
        a_ptrs += BLOCK_K * stride_ak
        b_ptrs += BLOCK_K * stride_bk

    c_ptrs = c_ptr + batch * stride_cb + rows[:, None] * stride_cm + cols[None, :] * stride_cn
    tl.store(c_ptrs, acc, mask=(rows[:, None] < M) & (cols[None, :] < N))


# The autotuner runs each configuration once per new (M, N, K, TF32) and keeps the fastest: the tile sizes that
# suit a 4096 x 4096 matmul make too few programs for a small one (the pipelined CUDA kernel's lesson)
CONFIGS = [
    triton.Config({"BLOCK_M": 128, "BLOCK_N": 128, "BLOCK_K": 32, "GROUP_M": 8}, num_warps=8, num_stages=3),
    triton.Config({"BLOCK_M": 128, "BLOCK_N": 256, "BLOCK_K": 32, "GROUP_M": 8}, num_warps=8, num_stages=3),
    triton.Config({"BLOCK_M": 256, "BLOCK_N": 128, "BLOCK_K": 32, "GROUP_M": 8}, num_warps=8, num_stages=3),
    triton.Config({"BLOCK_M": 128, "BLOCK_N": 64, "BLOCK_K": 32, "GROUP_M": 8}, num_warps=4, num_stages=4),
    triton.Config({"BLOCK_M": 64, "BLOCK_N": 128, "BLOCK_K": 32, "GROUP_M": 8}, num_warps=4, num_stages=4),
    triton.Config({"BLOCK_M": 64, "BLOCK_N": 64, "BLOCK_K": 32, "GROUP_M": 8}, num_warps=4, num_stages=4),
    triton.Config({"BLOCK_M": 128, "BLOCK_N": 128, "BLOCK_K": 16, "GROUP_M": 8}, num_warps=8, num_stages=4),
]
matmul_tuned = triton.autotune(configs=CONFIGS, key=["M", "N", "K", "TF32"])(matmul_kernel)


def strides(t):
    """A 3-D tensor's (batch, row, column) strides; a 2-D one is a batch of one, and an expanded one has stride 0."""
    return (t.stride(0), t.stride(1), t.stride(2)) if t.dim() == 3 else (0, t.stride(0), t.stride(1))


def matmul(a, b, tf32=False, config=None):
    """C = A @ B for (M, K) or (batch, M, K) A and (K, N) or (batch, K, N) B. With config, that one configuration
    is compiled and the compiled kernel is returned too (for its IR); otherwise the autotuner picks."""
    M, K = a.shape[-2:]
    N = b.shape[-1]
    batch = max(a.shape[0] if a.dim() == 3 else 1, b.shape[0] if b.dim() == 3 else 1)
    c = torch.empty((batch, M, N), device=a.device, dtype=torch.float32)
    args = (a, b, c, M, N, K, *strides(a), *strides(b), *strides(c))
    if config is None:
        grid = lambda meta: (triton.cdiv(M, meta["BLOCK_M"]) * triton.cdiv(N, meta["BLOCK_N"]), batch)
        matmul_tuned[grid](*args, TF32=tf32)
        return c, None
    kw = dict(config.kwargs)
    grid = (triton.cdiv(M, kw["BLOCK_M"]) * triton.cdiv(N, kw["BLOCK_N"]), batch)
    compiled = matmul_kernel[grid](*args, **kw, TF32=tf32, num_warps=config.num_warps, num_stages=config.num_stages)
    return c, compiled


def cublas(a, b, tf32):
    torch.backends.cuda.matmul.allow_tf32 = tf32  # cuBLAS's own FP32 or TF32 (tensor-core) path
    return torch.matmul(a, b)


def make(M, N, K, batch, one_a, device):
    g = torch.Generator(device="cpu").manual_seed(2024)
    a = torch.rand((1 if one_a else batch, M, K), generator=g) * 2 - 1
    b = torch.rand((batch, K, N), generator=g) * 2 - 1
    a, b = a.to(device), b.to(device)
    if one_a:
        a = a.expand(batch, M, K)  # stride 0 along the batch: one A for every product
    return a, b


def relative_error(c, ref):
    return ((c - ref).abs().max() / ref.abs().max().clamp_min(1e-30)).item()


CASES = [  # bench.cu's cases: M, N, K, batch, one A for the batch
    (1024, 1024, 1024, 1, False),
    (4096, 4096, 4096, 1, False),
    (1000, 999, 1001, 1, False),
    (256, 256, 256, 64, False),
    (512, 512, 512, 32, True),
]


def benchmark():
    print(f"{torch.cuda.get_device_name()}  torch {torch.__version__}  triton {triton.__version__}")
    for M, N, K, batch, one_a in CASES:
        a, b = make(M, N, K, batch, one_a, "cuda")
        flops = 2.0 * M * N * K * batch
        ref = cublas(a, b, False)
        print(f"\nM={M} N={N} K={K} batch={batch}{' (one A for the batch)' if one_a else ''}")
        for tf32 in (False, True):
            kind = "TF32" if tf32 else "FP32"
            ms_cublas = triton.testing.do_bench(lambda: cublas(a, b, tf32))
            c, _ = matmul(a, b, tf32)
            err = relative_error(c, ref)
            ok = err < (1e-2 if tf32 else 1e-4)  # TF32 rounds inputs to 10-bit mantissas
            ms = triton.testing.do_bench(lambda: matmul(a, b, tf32))
            best = matmul_tuned.best_config
            print(f"  cuBLAS {kind}  {ms_cublas:8.3f} ms {flops / ms_cublas / 1e9:9.1f} GFLOP/s")
            print(f"  Triton {kind}  {ms:8.3f} ms {flops / ms / 1e9:9.1f} GFLOP/s {100 * ms_cublas / ms:6.1f}% of cuBLAS"
                  f"   error {err:.1e} {'ok' if ok else 'WRONG'}   [{best.kwargs['BLOCK_M']}x{best.kwargs['BLOCK_N']}"
                  f"x{best.kwargs['BLOCK_K']}, {best.num_warps} warps, {best.num_stages} stages]")
    torch.backends.cuda.matmul.allow_tf32 = False


def dump_ir(out_dir):
    """One configuration, compiled for a 4096 x 4096 matmul in FP32 and in TF32, with the IR of every stage"""
    os.makedirs(out_dir, exist_ok=True)
    a, b = make(4096, 4096, 4096, 1, False, "cuda")
    config = CONFIGS[0]  # 128 x 128 x 32 tiles, 8 warps, 3 stages: the pipelined CUDA kernel's tile, deeper
    for tf32 in (False, True):
        _, compiled = matmul(a, b, tf32, config)
        for stage in ("ttir", "ttgir", "llir", "ptx"):
            path = os.path.join(out_dir, f"matmul_{'tf32' if tf32 else 'fp32'}.{stage}")
            with open(path, "w") as f:
                f.write(compiled.asm[stage])
            print(f"wrote {path} ({len(compiled.asm[stage].splitlines())} lines)")


def check():
    """The kernel's results on small cases, against torch.matmul: odd sizes, a batch, and one A for the batch"""
    device = "cuda" if torch.cuda.is_available() and not os.environ.get("TRITON_INTERPRET") else "cpu"
    config = triton.Config({"BLOCK_M": 32, "BLOCK_N": 32, "BLOCK_K": 16, "GROUP_M": 4}, num_warps=4, num_stages=2)
    failures = 0
    for M, N, K, batch, one_a in [(64, 64, 64, 1, False), (37, 45, 29, 3, False), (70, 130, 17, 2, True),
                                  (1, 65, 9, 1, False)]:
        a, b = make(M, N, K, batch, one_a, device)
        c, _ = matmul(a, b, False, config)
        err = relative_error(c, torch.matmul(a, b))
        failures += err >= 1e-4
        print(f"M={M:<4} N={N:<4} K={K:<4} batch={batch}{' (one A)' if one_a else ''}  error {err:.1e}"
              f"  {'ok' if err < 1e-4 else 'FAIL'}")
    print("all ok" if not failures else f"FAILURES: {failures}")
    return failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dump-ir", metavar="DIR")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        sys.exit(1 if check() else 0)
    elif args.dump_ir:
        dump_ir(args.dump_ir)
    else:
        benchmark()
