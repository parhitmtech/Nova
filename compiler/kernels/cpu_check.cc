// The kernels' results checked on the CPU, without a GPU or nvcc (`make cpu-check`): the kernels' own code runs with
// each CUDA thread of a block as a std::thread, the blocks one after another, __shared__ arrays as statics (shared
// by the block that is running) and __syncthreads as a barrier. It checks the indexing, the edges and the batch
// strides against a plain loop; speed is measured on a GPU (bench.cu).
#include <barrier>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

struct dim3
{
    unsigned x, y, z;
    dim3(unsigned x = 1, unsigned y = 1, unsigned z = 1) : x(x), y(y), z(z) {}
};
thread_local dim3 threadIdx, blockIdx;
dim3 blockDim, gridDim;
std::barrier<>* emuBarrier = nullptr;
#define __global__
#define __shared__ static
inline void __syncthreads() { emuBarrier->arrive_and_wait(); }

#include "matmul_kernels.cuh"

void launch(dim3 grid, dim3 block, const std::function<void()>& kernel)
{
    blockDim = block;
    gridDim = grid;
    int n = block.x * block.y * block.z;
    for (unsigned bz = 0; bz < grid.z; bz++)
        for (unsigned by = 0; by < grid.y; by++)
            for (unsigned bx = 0; bx < grid.x; bx++)
            {
                std::barrier<> bar(n);
                emuBarrier = &bar;
                std::vector<std::thread> threads;
                for (unsigned tz = 0; tz < block.z; tz++)
                    for (unsigned ty = 0; ty < block.y; ty++)
                        for (unsigned tx = 0; tx < block.x; tx++)
                            threads.emplace_back([&, tx, ty, tz, bx, by, bz] {
                                threadIdx = dim3(tx, ty, tz);
                                blockIdx = dim3(bx, by, bz);
                                kernel();
                            });
                for (auto& t : threads) t.join();
                emuBarrier = nullptr;
            }
}

int failures = 0;

void check(const char* kernel, int M, int N, int K, int batch, bool broadcastA)
{
    long long sA = broadcastA ? 0 : (long long)M * K, sB = (long long)K * N, sC = (long long)M * N;
    std::vector<float> A((broadcastA ? 1 : batch) * M * K), B(batch * K * N), C(batch * M * N, -7.0f), R(C.size());
    unsigned s = 12345;
    auto rnd = [&] { s = s * 1103515245u + 12345u; return ((s >> 8) % 2001) / 1000.0f - 1.0f; };
    for (float& v : A) v = rnd();
    for (float& v : B) v = rnd();
    for (int z = 0; z < batch; z++)
        for (int i = 0; i < M; i++)
            for (int j = 0; j < N; j++)
            {
                double sum = 0;
                for (int t = 0; t < K; t++) sum += A[z * sA + i * K + t] * B[z * sB + t * N + j];
                R[z * sC + i * N + j] = (float)sum;
            }
    const float *a = A.data(), *b = B.data();
    float* c = C.data();
    std::string name = kernel;
    if (name == "naive")
        launch(dim3((N + 15) / 16, (M + 15) / 16, batch), dim3(16, 16),
               [&] { matmul_naive(a, b, c, M, N, K, sA, sB, sC); });
    else if (name == "tiled")
        launch(dim3((N + 31) / 32, (M + 31) / 32, batch), dim3(32, 32),
               [&] { matmul_tiled<32>(a, b, c, M, N, K, sA, sB, sC); });
    else
        launch(dim3((N + 63) / 64, (M + 63) / 64, batch), dim3(256),
               [&] { matmul_blocked<64, 64, 8, 4, 4>(a, b, c, M, N, K, sA, sB, sC); });
    double worst = 0;
    for (size_t i = 0; i < C.size(); i++) worst = std::fmax(worst, std::fabs(C[i] - R[i]));
    bool ok = worst < 1e-3;
    failures += !ok;
    printf("%-8s M=%-4d N=%-4d K=%-4d batch=%d%s  max diff %.2e  %s\n", kernel, M, N, K, batch,
           broadcastA ? " (A broadcast)" : "", worst, ok ? "ok" : "FAIL");
}

int main()
{
    for (const char* k : {"naive", "tiled", "blocked"})
    {
        check(k, 64, 64, 64, 1, false);
        check(k, 37, 45, 29, 3, false);
        check(k, 70, 130, 17, 2, true);
        check(k, 1, 65, 9, 1, false);
    }
    printf(failures ? "FAILURES: %d\n" : "all ok\n", failures);
    return failures != 0;
}
