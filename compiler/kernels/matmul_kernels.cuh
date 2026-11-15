#ifndef NOVA_MATMUL_KERNELS_CUH
#define NOVA_MATMUL_KERNELS_CUH

// Matrix multiply kernels for Nova's matmul and batched matmul (bmm), in the three classic steps: one thread per
// element, shared-memory tiles, and register blocking. Each takes row-major float matrices, C = A @ B with A (M x K),
// B (K x N) and C (M x N), and a batch of them along the grid's z: product z uses A + z * strideA, B + z * strideB
// and C + z * strideC. A stride of 0 uses one matrix for the whole batch, as Nova's (n, k) @ (b, k, m) does.

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

#endif  // NOVA_MATMUL_KERNELS_CUH
