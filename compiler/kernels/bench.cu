// Benchmark of the matmul kernels (matmul_kernels.cuh) against cuBLAS: for each case, each kernel's result is
// checked against cuBLAS's, then timed. Prints the time, GFLOP/s and the share of cuBLAS's speed.
//
//   ./bench                  the default cases
//   ./bench M N K [batch] [broadcast]
//                            one case; "broadcast" uses one A for the whole batch
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cublas_v2.h>
#include <cuda_runtime.h>
#include "matmul_kernels.cuh"

#define CUDA_CHECK(call)                                                                                      \
    do {                                                                                                      \
        cudaError_t e = (call);                                                                               \
        if (e != cudaSuccess)                                                                                 \
        {                                                                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e));                        \
            exit(1);                                                                                          \
        }                                                                                                     \
    } while (0)
#define CUBLAS_CHECK(call)                                                                                    \
    do {                                                                                                      \
        cublasStatus_t s = (call);                                                                            \
        if (s != CUBLAS_STATUS_SUCCESS)                                                                       \
        {                                                                                                     \
            fprintf(stderr, "%s:%d: cuBLAS error %d\n", __FILE__, __LINE__, (int)s);                          \
            exit(1);                                                                                          \
        }                                                                                                     \
    } while (0)

struct Case
{
    int M, N, K, batch;
    bool broadcastA;  // one A for every product of the batch (stride 0)
};

// One batched product on the device: A, B and C with their batch strides
struct Problem
{
    Case c;
    const float* A;
    const float* B;
    float* C;
    long long sA, sB, sC;
};

void launch_naive(const Problem& p)
{
    dim3 block(16, 16);
    dim3 grid((p.c.N + 15) / 16, (p.c.M + 15) / 16, p.c.batch);
    matmul_naive<<<grid, block>>>(p.A, p.B, p.C, p.c.M, p.c.N, p.c.K, p.sA, p.sB, p.sC);
}

void launch_tiled(const Problem& p)
{
    constexpr int TILE = 32;
    dim3 block(TILE, TILE);
    dim3 grid((p.c.N + TILE - 1) / TILE, (p.c.M + TILE - 1) / TILE, p.c.batch);
    matmul_tiled<TILE><<<grid, block>>>(p.A, p.B, p.C, p.c.M, p.c.N, p.c.K, p.sA, p.sB, p.sC);
}

void launch_blocked(const Problem& p)
{
    constexpr int BM = 64, BN = 64, BK = 8, TM = 4, TN = 4;
    dim3 block((BM / TM) * (BN / TN));
    dim3 grid((p.c.N + BN - 1) / BN, (p.c.M + BM - 1) / BM, p.c.batch);
    matmul_blocked<BM, BN, BK, TM, TN><<<grid, block>>>(p.A, p.B, p.C, p.c.M, p.c.N, p.c.K, p.sA, p.sB, p.sC);
}

void launch_pipelined(const Problem& p)
{
    dim3 block(256);
    dim3 grid((p.c.N + 127) / 128, (p.c.M + 127) / 128, p.c.batch);
    // 16-byte copies need every row of A and B to start on a 16-byte boundary: K and N multiples of 4
    if (p.c.K % 4 == 0 && p.c.N % 4 == 0)
    {
        matmul_pipelined<true><<<grid, block>>>(p.A, p.B, p.C, p.c.M, p.c.N, p.c.K, p.sA, p.sB, p.sC);
    }
    else
    {
        matmul_pipelined<false><<<grid, block>>>(p.A, p.B, p.C, p.c.M, p.c.N, p.c.K, p.sA, p.sB, p.sC);
    }
}

// cuBLAS is column-major: a row-major C = A @ B is the column-major C^T = B^T @ A^T, so B goes first
cublasHandle_t handle;
void launch_cublas(const Problem& p)
{
    const float one = 1.0f, zero = 0.0f;
    CUBLAS_CHECK(cublasSgemmStridedBatched(handle, CUBLAS_OP_N, CUBLAS_OP_N, p.c.N, p.c.M, p.c.K, &one,
                                           p.B, p.c.N, p.sB, p.A, p.c.K, p.sA, &zero, p.C, p.c.N, p.sC,
                                           p.c.batch));
}

// Average time of one run, after a warm-up run
template <class F>
float time_ms(F run, int iters)
{
    run();
    CUDA_CHECK(cudaDeviceSynchronize());
    cudaEvent_t start, stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));
    CUDA_CHECK(cudaEventRecord(start));
    for (int i = 0; i < iters; i++) run();
    CUDA_CHECK(cudaEventRecord(stop));
    CUDA_CHECK(cudaEventSynchronize(stop));
    float ms = 0;
    CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));
    return ms / iters;
}

int failures = 0;

void run_case(const Case& c)
{
    size_t aCount = (size_t)(c.broadcastA ? 1 : c.batch) * c.M * c.K;
    size_t bCount = (size_t)c.batch * c.K * c.N, cCount = (size_t)c.batch * c.M * c.N;
    std::vector<float> hA(aCount), hB(bCount), hRef(cCount), hC(cCount);
    unsigned s = 2024;
    auto rnd = [&] { s = s * 1103515245u + 12345u; return ((s >> 8) % 2001) / 1000.0f - 1.0f; };
    for (float& v : hA) v = rnd();
    for (float& v : hB) v = rnd();

    float *A, *B, *C, *Ref;
    CUDA_CHECK(cudaMalloc(&A, aCount * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&B, bCount * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&C, cCount * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&Ref, cCount * sizeof(float)));
    CUDA_CHECK(cudaMemcpy(A, hA.data(), aCount * sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(B, hB.data(), bCount * sizeof(float), cudaMemcpyHostToDevice));
    long long sA = c.broadcastA ? 0 : (long long)c.M * c.K, sB = (long long)c.K * c.N, sC = (long long)c.M * c.N;

    Problem ref{c, A, B, Ref, sA, sB, sC};
    launch_cublas(ref);
    CUDA_CHECK(cudaMemcpy(hRef.data(), Ref, cCount * sizeof(float), cudaMemcpyDeviceToHost));
    double scale = 0;
    for (float v : hRef) scale = std::fmax(scale, std::fabs(v));

    double flops = 2.0 * c.M * c.N * c.K * c.batch;
    int iters = flops > 1e10 ? 5 : 20;
    printf("\nM=%d N=%d K=%d batch=%d%s\n", c.M, c.N, c.K, c.batch, c.broadcastA ? " (one A for the batch)" : "");
    float cublasMs = time_ms([&] { launch_cublas(ref); }, iters);
    printf("  %-9s %9.3f ms %9.1f GFLOP/s\n", "cuBLAS", cublasMs, flops / cublasMs / 1e6);

    struct Kernel
    {
        const char* name;
        void (*launch)(const Problem&);
    };
    for (Kernel k : {Kernel{"naive", launch_naive}, Kernel{"tiled", launch_tiled}, Kernel{"blocked", launch_blocked},
                     Kernel{"pipelined", launch_pipelined}})
    {
        Problem p{c, A, B, C, sA, sB, sC};
        CUDA_CHECK(cudaMemset(C, 0, cCount * sizeof(float)));
        k.launch(p);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaMemcpy(hC.data(), C, cCount * sizeof(float), cudaMemcpyDeviceToHost));
        double worst = 0;
        for (size_t i = 0; i < cCount; i++) worst = std::fmax(worst, std::fabs(hC[i] - hRef[i]));
        double relative = scale > 0 ? worst / scale : worst;  // float sums in another order differ a little
        bool ok = relative < 1e-4;
        failures += !ok;
        float ms = time_ms([&] { k.launch(p); }, iters);
        printf("  %-9s %9.3f ms %9.1f GFLOP/s %6.1f%% of cuBLAS   error %.1e %s\n", k.name, ms, flops / ms / 1e6,
               100.0 * cublasMs / ms, relative, ok ? "ok" : "WRONG");
    }
    CUDA_CHECK(cudaFree(A));
    CUDA_CHECK(cudaFree(B));
    CUDA_CHECK(cudaFree(C));
    CUDA_CHECK(cudaFree(Ref));
}

int main(int argc, char** argv)
{
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("%s (compute capability %d.%d, %d SMs)\n", prop.name, prop.major, prop.minor, prop.multiProcessorCount);
    CUBLAS_CHECK(cublasCreate(&handle));

    std::vector<Case> cases;
    if (argc >= 4)
    {
        Case c{atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), argc >= 5 ? atoi(argv[4]) : 1,
               argc >= 6 && strcmp(argv[5], "broadcast") == 0};
        cases.push_back(c);
    }
    else
    {
        cases = {
            {1024, 1024, 1024, 1, false},   // square
            {4096, 4096, 4096, 1, false},   // large enough to be compute-bound
            {1000, 999, 1001, 1, false},    // sizes that are no multiple of a tile
            {256, 256, 256, 64, false},     // a batch of small products (bmm)
            {512, 512, 512, 32, true},      // one A for the batch: (n, k) @ (b, k, m)
        };
    }
    for (const Case& c : cases) run_case(c);
    CUBLAS_CHECK(cublasDestroy(handle));
    printf(failures ? "\n%d result(s) WRONG\n" : "\nall results match cuBLAS\n", failures);
    return failures != 0;
}
