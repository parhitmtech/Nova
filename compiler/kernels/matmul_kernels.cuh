#ifndef NOVA_MATMUL_KERNELS_CUH
#define NOVA_MATMUL_KERNELS_CUH

// Matrix multiply kernels for Nova's matmul and batched matmul (bmm), in the classic steps: one thread per element,
// shared-memory tiles, register blocking, and a pipelined kernel that loads the next tiles while using the current
// ones. Each takes row-major float matrices, C = A @ B with A (M x K), B (K x N) and C (M x N), and a batch of them
// along the grid's z: product z uses A + z * strideA, B + z * strideB and C + z * strideC. A stride of 0 uses one
// matrix for the whole batch, as Nova's (n, k) @ (b, k, m) does.

// 1. Naive: one thread per element of C, reading its row of A and its column of B from global memory. Neighbouring
// threads of a warp take neighbouring columns, so their reads of B and their writes of C are coalesced; every read
// of A and B still goes to global memory, K times per element.
__global__ void matmul_naive(const float* __restrict__ A, const float* __restrict__ B, float* __restrict__ C,
                             int M, int N, int K, long long strideA, long long strideB, long long strideC)
{
    A += blockIdx.z * strideA;
    B += blockIdx.z * strideB;
    C += blockIdx.z * strideC;
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= M || col >= N) return;
    float sum = 0.0f;
    for (int t = 0; t < K; t++) sum += A[(long long)row * K + t] * B[(long long)t * N + col];
    C[(long long)row * N + col] = sum;
}

// 2. Tiled: a TILE x TILE block of threads computes a TILE x TILE tile of C. It walks along K a tile at a time; each
// thread loads one element of A's tile and one of B's into shared memory, and the block then multiplies the two
// tiles from there, so each element read from global memory is used TILE times. Elements past the edge of a matrix
// load as 0, so any M, N and K work.
template <int TILE>
__global__ void matmul_tiled(const float* __restrict__ A, const float* __restrict__ B, float* __restrict__ C,
                             int M, int N, int K, long long strideA, long long strideB, long long strideC)
{
    __shared__ float As[TILE][TILE];
    __shared__ float Bs[TILE][TILE];
    A += blockIdx.z * strideA;
    B += blockIdx.z * strideB;
    C += blockIdx.z * strideC;
    int tx = threadIdx.x, ty = threadIdx.y;
    int row = blockIdx.y * TILE + ty;
    int col = blockIdx.x * TILE + tx;
    float sum = 0.0f;
    for (int k0 = 0; k0 < K; k0 += TILE)  // no thread returns early: every thread must reach each __syncthreads
    {
        As[ty][tx] = (row < M && k0 + tx < K) ? A[(long long)row * K + k0 + tx] : 0.0f;
        Bs[ty][tx] = (k0 + ty < K && col < N) ? B[(long long)(k0 + ty) * N + col] : 0.0f;
        __syncthreads();
        for (int t = 0; t < TILE; t++) sum += As[ty][t] * Bs[t][tx];
        __syncthreads();  // the tiles are overwritten by the next step
    }
    if (row < M && col < N) C[(long long)row * N + col] = sum;
}

// 3. Register blocking: a block of (BM / TM) * (BN / TN) threads computes a BM x BN tile of C, each thread a TM x TN
// piece of it held in registers. For each step of BK along K, the block loads A's BM x BK tile (transposed, so a
// thread's TM rows of one column sit side by side) and B's BK x BN tile into shared memory; then for each of the BK
// columns a thread reads TM values of A and TN of B into registers and does TM * TN multiply-adds with them. Each
// value read from shared memory is used TN or TM times, where the tiled kernel used it once.
template <int BM, int BN, int BK, int TM, int TN>
__global__ void matmul_blocked(const float* __restrict__ A, const float* __restrict__ B, float* __restrict__ C,
                               int M, int N, int K, long long strideA, long long strideB, long long strideC)
{
    constexpr int THREADS = (BM / TM) * (BN / TN);
    __shared__ float As[BK][BM];
    __shared__ float Bs[BK][BN];
    A += blockIdx.z * strideA;
    B += blockIdx.z * strideB;
    C += blockIdx.z * strideC;
    int tid = threadIdx.x;
    int tRow = tid / (BN / TN), tCol = tid % (BN / TN);  // this thread's piece of the tile
    int row0 = blockIdx.y * BM, col0 = blockIdx.x * BN;
    float acc[TM][TN] = {};
    float a[TM], b[TN];
    for (int k0 = 0; k0 < K; k0 += BK)
    {
        for (int i = tid; i < BM * BK; i += THREADS)
        {
            int r = i / BK, c = i % BK;
            As[c][r] = (row0 + r < M && k0 + c < K) ? A[(long long)(row0 + r) * K + k0 + c] : 0.0f;
        }
        for (int i = tid; i < BK * BN; i += THREADS)
        {
            int r = i / BN, c = i % BN;
            Bs[r][c] = (k0 + r < K && col0 + c < N) ? B[(long long)(k0 + r) * N + col0 + c] : 0.0f;
        }
        __syncthreads();
        for (int t = 0; t < BK; t++)
        {
            for (int i = 0; i < TM; i++) a[i] = As[t][tRow * TM + i];
            for (int j = 0; j < TN; j++) b[j] = Bs[t][tCol * TN + j];
            for (int i = 0; i < TM; i++)
            {
                for (int j = 0; j < TN; j++) acc[i][j] += a[i] * b[j];
            }
        }
        __syncthreads();
    }
    for (int i = 0; i < TM; i++)
    {
        for (int j = 0; j < TN; j++)
        {
            int r = row0 + tRow * TM + i, c = col0 + tCol * TN + j;
            if (r < M && c < N) C[(long long)r * N + c] = acc[i][j];
        }
    }
}

// Asynchronous copies from global to shared memory (cp.async, compute capability 8.0 and later): a thread starts a
// copy and goes on computing; commit closes a group of copies, and wait<N> waits until at most N groups are still in
// flight. A copy whose source is past the edge of a matrix copies 0 bytes and fills its destination with zeros.
// Without a GPU (cpu_check.cc) each copy happens at once.
__device__ __forceinline__ void cp_async_16(float* dst, const float* src, bool valid)
{
#if defined(__CUDA_ARCH__)
    unsigned s = static_cast<unsigned>(__cvta_generic_to_shared(dst));
    asm volatile("cp.async.cg.shared.global [%0], [%1], 16, %2;\n" ::"r"(s), "l"(src), "r"(valid ? 16 : 0));
#else
    for (int q = 0; q < 4; q++) dst[q] = valid ? src[q] : 0.0f;
#endif
}

__device__ __forceinline__ void cp_async_4(float* dst, const float* src, bool valid)
{
#if defined(__CUDA_ARCH__)
    unsigned s = static_cast<unsigned>(__cvta_generic_to_shared(dst));
    asm volatile("cp.async.ca.shared.global [%0], [%1], 4, %2;\n" ::"r"(s), "l"(src), "r"(valid ? 4 : 0));
#else
    *dst = valid ? *src : 0.0f;
#endif
}

__device__ __forceinline__ void cp_async_commit()
{
#if defined(__CUDA_ARCH__)
    asm volatile("cp.async.commit_group;\n" ::);
#endif
}

template <int N>
__device__ __forceinline__ void cp_async_wait()
{
#if defined(__CUDA_ARCH__)
    asm volatile("cp.async.wait_group %0;\n" ::"n"(N) : "memory");
#endif
}

// 4. Pipelined: register blocking at a larger scale, with the next tiles loaded while the current ones are used.
// A block of 256 threads computes a 128 x 128 tile of C, each thread 8 x 8 of it (64 accumulators), in steps of 8
// along K. Shared memory holds two copies of each tile (double buffering): while the block multiplies the tiles of
// step k from one copy, cp.async fills the other with step k + 1's, so loading from global memory overlaps with the
// arithmetic instead of stalling it. With VEC each thread loads 4 neighbouring floats of A and of B in one 16-byte
// copy; it needs K and N to be multiples of 4 (so every 16-byte source is aligned), and the scalar copies serve the
// other sizes. A thread's 8 rows are two groups of 4 (rows r and 64 + r) and so are its 8 columns, so that the 16
// threads reading a row of B's tile read 64 consecutive floats as float4s, and none share a bank.
template <bool VEC>
__global__ void __launch_bounds__(256) matmul_pipelined(const float* __restrict__ A, const float* __restrict__ B,
                                                        float* __restrict__ C, int M, int N, int K,
                                                        long long strideA, long long strideB, long long strideC)
{
    constexpr int BM = 128, BN = 128, BK = 8, PAD = 4;
    __shared__ __align__(16) float As[2][BM][BK + PAD];  // A's tile as it is in memory; PAD keeps rows r and r + 4
    __shared__ __align__(16) float Bs[2][BK][BN];        // in different banks, and rows 16-byte aligned
    A += blockIdx.z * strideA;
    B += blockIdx.z * strideB;
    C += blockIdx.z * strideC;
    int tid = threadIdx.x;
    int tRow = tid / 16, tCol = tid % 16;  // this thread's rows tRow * 4 + (0..3) and 64 + those; columns likewise
    int row0 = blockIdx.y * BM, col0 = blockIdx.x * BN;
    int aRow = tid / 2, aCol = (tid % 2) * 4;    // the 4 floats of A's tile this thread loads
    int bRow = tid / 32, bCol = (tid % 32) * 4;  // and of B's

    auto load = [&](int stage, int k0)
    {
        const float* a = A + (long long)(row0 + aRow) * K + k0 + aCol;
        const float* b = B + (long long)(k0 + bRow) * N + col0 + bCol;
        if (VEC)
        {
            cp_async_16(&As[stage][aRow][aCol], row0 + aRow < M && k0 + aCol < K ? a : A,
                        row0 + aRow < M && k0 + aCol < K);
            cp_async_16(&Bs[stage][bRow][bCol], k0 + bRow < K && col0 + bCol < N ? b : B,
                        k0 + bRow < K && col0 + bCol < N);
        }
        else
        {
            for (int q = 0; q < 4; q++)
            {
                bool aIn = row0 + aRow < M && k0 + aCol + q < K;
                bool bIn = k0 + bRow < K && col0 + bCol + q < N;
                cp_async_4(&As[stage][aRow][aCol + q], aIn ? a + q : A, aIn);
                cp_async_4(&Bs[stage][bRow][bCol + q], bIn ? b + q : B, bIn);
            }
        }
    };

    float acc[8][8] = {};
    int steps = (K + BK - 1) / BK;
    load(0, 0);
    cp_async_commit();
    for (int s = 0; s < steps; s++)
    {
        int cur = s % 2;
        if (s + 1 < steps) load(cur ^ 1, (s + 1) * BK);  // the other copy was last read in step s - 1
        cp_async_commit();   // a group every step, empty or not, so that wait<1> always means "all but the newest"
        cp_async_wait<1>();  // step s's tiles have arrived (step s + 1's may still be on their way)
        __syncthreads();     // ... for every thread's copies, not only this thread's
        for (int t = 0; t < BK; t++)
        {
            float a[8], b[8];
            for (int i = 0; i < 4; i++)
            {
                a[i] = As[cur][tRow * 4 + i][t];
                a[4 + i] = As[cur][64 + tRow * 4 + i][t];
            }
            float4 b0 = *reinterpret_cast<const float4*>(&Bs[cur][t][tCol * 4]);
            float4 b1 = *reinterpret_cast<const float4*>(&Bs[cur][t][64 + tCol * 4]);
            b[0] = b0.x; b[1] = b0.y; b[2] = b0.z; b[3] = b0.w;
            b[4] = b1.x; b[5] = b1.y; b[6] = b1.z; b[7] = b1.w;
            for (int i = 0; i < 8; i++)
            {
                for (int j = 0; j < 8; j++) acc[i][j] += a[i] * b[j];
            }
        }
        __syncthreads();  // every thread is done with copy cur before step s + 1 starts loading step s + 2 into it
    }
    for (int i = 0; i < 8; i++)
    {
        int r = row0 + (i < 4 ? tRow * 4 + i : 64 + tRow * 4 + i - 4);
        for (int j = 0; j < 8; j++)
        {
            int c = col0 + (j < 4 ? tCol * 4 + j : 64 + tCol * 4 + j - 4);
            if (r < M && c < N) C[(long long)r * N + c] = acc[i][j];
        }
    }
}

#endif  // NOVA_MATMUL_KERNELS_CUH
