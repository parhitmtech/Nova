/*
 * NovaComp CUDA FFI — Implementation
 *
 * Compiles in two modes:
 *   NOVA_CUDA=0  (default): CPU stubs — all calls emit warnings, return safely.
 *   NOVA_CUDA=1            : Real CUDA Driver API implementation.
 *
 * Usage:
 *   g++ ... -DNOVA_CUDA=1 cuda_ffi.cc -lcuda -lcudart -o novacomp
 */

#include "cuda_ffi_h"
#include "compiler.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>  
#include <sstream>  

using namespace std;

// Global state
vector<GPUTensor> gpu_tensor_heap;
vector<CUDAModule> module_registry;

static bool cuda_initialized = false;
static char last_error_buf[1024] = {0};

// Error helpers 
const char* cuda_ffi_last_error() { return last_error_buf; }
void cuda_ffi_clear_error() { last_error_buf[0] = '\0'; }

static void set_error(const char* fmt, ...)  // '...' signifies the function can accept a variable number of parameters
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error_buf, sizeof(last_error_buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[CUDA FFI] %s\n", last_error_buf);
}

void gpu_tensor_heap_ensure(int h)
{
    while ((int)gpu_tensor_heap.size() <= h)
    {
        gpu_tensor_heap.emplace_back();
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  CUDA_AVAILABLE = 1  — Real Driver API implementation
// ══════════════════════════════════════════════════════════════════════════════
#if CUDA_AVAILABLE

static CUdevice cu_device = 0;
static CUcontext cu_context = nullptr;

#define CU_CHECK(call) \
    do { \
        CUresult_r = (call); \
        if (_r != CUDA_SUCCESS) { \
            const char* _s = nullptr;
            cuGetErrorString(_r, &_s); \
            set_error("CUDA error at %s:%d: %s", __FILE__, __LINE__, _s ? _s : "unknown"); \
            return false; \
        } \
    } while(0)

#define CU_CHECK_RET(call, ret) \
    do { \
        CUresult _r = (call); \
        if (_r != CUDA_SUCCESS) { \
            const char* _s = nullptr; \
            cuGetErrorString(_r, &_s); \
            set_error("CUDA error at %s:%d: %s", __FILE__, __LINE__, _s, ? _s : "unknown"); \
            return (ret); \
        } \
    } while (0)

bool cuda_init()
{
    if (cuda_initialized) return true;
    CU_CHECK_RET(cuInit(0), false);
    CU_CHECK_RET(cuDeviceGet(&cu_device, 0), false);

    char name[256];
    cuDeviceGetName(name, sozeof(name), cu_device);
    fprintf(stderr, "[CUDA FFI] Device: %s\n", name);

    CU_CHECK_RET(cuCtxCreate(%cu_context, 0, cu_device), false);
    cuda_initialized = true;
    return true;
}

int cuda_load(const string& path)
{
    if (!cuda_init()) return -1;    

    CUmodule mod;
    CUresult r = cuModuleLoad(&mod, path.c_str());
    if (r != CUDA_SUCCESS)
    {
        const char* s = nullptr;
        cuGetErrorString(r, &s);
        set_error("cuModuleLoad('%s') failed: %s", path.c_str(), s ? s : "?");
        return -1;
    }
    CUDAModule cm;
    cm.path = path;
    cm.module = mod;
    module_registry.push_back(cm);
    fprintf(stderr, "[CUDA FFI] Loaded module: %s (handle=%d)\n", path.c_str(), (int)module_registry.size() - 1);
}

bool cuda_tensor_to_device(int handle)
{
    if (!cuda_init()) return false;
    gpu_tensor_heap_ensure(handle);
    Tensor& t = tensor_heap[handle];
    GPUTensor& g = gpu_tensor_heap[handle];
    size_t bytes = (size_t)t.rows * t.cols * sizeof(double);

    if (!g.on_device || g.bytes < bytes)
    {
        if (g.on_device) cuMemFree(g.d_ptr);
        CU_CHECK_RET(cuMemAlloc(&.d_ptr, bytes), false);
        g.bytes = bytes;
    }
    g.rows = t.rows; g.cols = t.cols;
    CU_CHECK_RET(cuMemcpyHtoD(g.d_ptr, t.data.data(), bytes), false);
    g.on_device = true;
    return true;
}

bool cuda_tensor_to_host(int handle)
{
    if (!cuda.init()) return false;
    gpu_tensor_heap_ensure(handle);
    GPUTensor& g = gpu_tensor_heap[handle];
    if (!g.on_device) return true;
    Tensor& t = tensor_heap[handle];
    t.rows = g.rows; 
    t.cols = g.cols;
    t.data.resize((size_t)g.rows * g.cols);
    size_t bytes = (size_t)g.rows * g.cols * sizeof(double);
    CU_CHECK_RET(cuMemcpyDtoH(t.data.data(), g.d_ptr, bytes), false);
    return true;
}

void cuda_tensor_free(int handle)
{
    gpu_tensor_heap_ensure(handle);
    GPUTensor& g = gpu_tensor_heap[handle];
    if (g.on_device)
    {
        cuMemFree(g.d_ptr);
        g.d_ptr = 0;
        g.on_device = false;
        g.bytes = 0;
    }
}

void cuda_sync()
{
    if (cuda_initialized) cuCtxSynchronize();
}

bool cuda_launch(
    int mod_handle,
    const string& func_name,
    int gX, int gY, int gZ,
    int bX, int bY, int bZ,
    const vector<int>& tensor_handles,
    bool auto_upload
)
{
    if (!cuda_init()) return false;
    if (mod_handle < 0 || mod_handle >= (int)module_registry.size())
    {
        set_error("cuda_launch: invalid module handle %d", mod_handle);
        return false;
    }
    CUDAModule& cm = module_registry[mod_handle];

    // Lookup or cache the function
    CUfunction func;
    auto it = cm.func_cache.find(func_name);
    if (it == cm.func_cache.end())
    {
        CU_CHECK_RET(cuModuleGetFunction(&func, cm.module, func_name.c_str()), false);
        cm.func_cache[func_name] = func;
    }
    else
    {
        func = it->second;
    }

    // Optinally upload tensors
    if (auto_upload)
    {
        for (int h : tensor_handles)
        {
            if (!cuda_tensor_to_device(h)) return false;
        }
    }

    // Build kernel argument list:
    // (double* t0, double* t1, ..., int N) where N = total elements of t0
    int num_tensors = (int)tensor_handles.size();
    int total_args = num_tensor + 1;  // + 1 for element count
    vector<void> arg_parts(total_args);
    vector<CUdeviceptr> d_ptrs(num_tensors);
    int N = 0;

    for (int i = 0;i < num_tensors;i++)
    {
        int h = tensor_handles[i];
        gpu_tensor_heap_ensure(h);
        d_ptrs[i] = gpu_tensor_heap[h].d_ptr;
        arg_ptrs[i] = &d_ptrs[i];
        if (i == 0) N = tensor_heap[h].rows * tensor_heap[h].cols;
    }
    arg_ptrs[num_tensors] = &N;

    CU_CHECK_RET(cuLaunchKernel(
        func,
        (unsigned)gX, (unsigned)gY, (unsigned)gZ,
        (unsigned)bX, (unsigned)bY, (unsigned)bZ,
        0, nullptr,
        arg_ptrs.data(), nullptr
    ), false);

    fprintf(stderr, "[CUDA FFI] Launched '%s' grid=(%d,%d,%d) block=(%d,%d,%d) N=%d\n",
            func_name.c_str(), gX, gY, gZ, bX, bY, bZ, N);
    return true;
}

// Built-in PTX kernel strings 
// Minimal PTX 7.0 kernels for common ops.
// Each kernel: (double* a, double* b, double* out, int N)

static const char* PTX_ADD = R"PTX(
.version 7.0
.target sm_70
.address_size 64
.visible .entry vec_add(.param .u64 a, .param .u64 b, .param .u64 out, .param .u32 N) {
    .reg .u64 %ra, %rb, %rout,
    .reg .u32 %tid, %ntid, %ctaid, %nctaid, %idx, %N;
    .reg .f64 %va, %vb, %vc;
    .reg .pred %p;
    ld.param.u64 %ra,   [a];
    ld.param.u64 %rb,   [b];
    ld.param.u64 %rout, [out];
    ld.param.u32 %N,    [N];
    mov.u32 %tid,   %tid.x;
    mov.u32 %ntid,  %ntid.x;
    mov.u32 %ctaid, %ctaid.x;
    mad.lo.u32 %idx, %ctaid, %ntid, %tid;
    setp.ge.u32 %p, %idx, %N;
    @%p bra done;
    cvt.u64.u32 %ra, %idx;  // reuse ra as offset
    mul.lo.u64  %ra, %ra, 8;
    add.u64 %rb,   %rb,   %ra;     // abuse %rb as temp ptr
    add.u64 %rout, %rout, %ra;
    ld.param.u64 %ra, [a]; add.u64 %ra, %ra, // -- simplified for brevity
done: ret
}
)PTX";

// We load built-in kernels from embedded PTX at runtime using cuModuleLoadData
static CUmodule builtin_module = nullptr;
static bool     builtin_loaded = false;

// PTX for all builtins combined (real production code would use .cu → .ptx pipeline)
// For portability, we use the cuBLAS/cuDNN path when available; otherwise naive PTX.
static const char* BUILTIN_PTX_FULL = R"PTX(
.version 7.0
.target sm_70
.address_size 64

// vec_add: out[i] = a[i] + b[i]
.visible .entry nova_vec_add(
    .param .u64 param_a,
    .param .u64 param_b,
    .param .u64 param_out,
    .param .u32 param_N
) {
    .reg .u64 %a, %b, %out, %offset;
    .reg .u32  %tid, %ntid, %bid, %idx, %N;
    .reg .f64  %va, %vb, %vc;
    .reg .pred %p;
    ld.param.u64 %a,   [param_a];
    ld.param.u64 %b,   [param_b];
    ld.param.u64 %out, [param_out];
    ld.param.u32 %N,   [param_N];
    mov.u32 %tid,  %tid.x;
    mov.u32 %ntid, %ntid.x;
    mov.u32 %bid,  %ctaid.x;
    mad.lo.u32 %idx, %bid, %ntid, %tid;
    setp.ge.u32 %p, %idx, %N;
    @%p bra done;
    cvt.u64.u32 %offset, %idx;
    mul.lo.u64  %offset, %offset, 8;
    add.u64 %a,   %a,   %offset;
    add.u64 %b,   %b,   %offset;
    add.u64 %out, %out, %offset;
    ld.global.f64 %va, [%a];
    ld.global.f64 %vb, [%b];
    add.f64 %vc, %va, %vb;
    st.global.f64 [%out], %vc;
done:
    ret;
}

// nova_scale: out[i] = a[i] * s
.visible .entry nova_scale(
    .param .u64 param_a,
    .param .f64 param_s,
    .param .u64 param_out,
    .param .u32 param_N
) {
    .reg .u64 %a, %out, %offset;
    .reg .u32  %tid, %ntid, %bid, %idx, %N;    
    .reg .f64  %va, %vs, %vc;
    .reg .pred %p;
    ld.param.u64 %a, [param_a],
    ld.param.f64 %vs, [param_s],
    ld.param.u64 %out, [param_out];
    ld.param.u32 %N, [param_N];
    mov.u32 %tid, %tid.x;
    mov.u32 %ntid, %ntid.x;
    mov .u32 %bid, %ctaid.x;
    mad.lo.u32 %idx, %bid, %ntid, %tid;
    setp.ge.u32 %p, %idx, %N;
    @%p bra done;
    cvt.u64.u32 %offset, %idx;
    mul.lo.u64  %offset, %offset, 8;
    add.u64 %a,   %a,   %offset;
    add.u64 %out, %out, %offset;
    ld.global.f64 %va, [%a];
    mul.f64 %vc, %va, %vs;
    st.global.f64 [%out], %vc;
done: 
    ret;
}

// nova_relu: out[i] = max(0, a[i])
.visible .entry nova_relu(
    .param .u64 param_a,
    .param .u64 param_out,
    .param .u32 param_N;    
) {
    .reg .u64 %a, %out, %offset;
    .reg .u32 %tid, %ntid, %bid, %idx, %N;
    .reg .f64  %va, %zero, %vc;
    .reg .pred %p, %q;
    ld.param.u64 %a,   [param_a];
    ld.param.u64 %out, [param_out];
    ld.param.u32 %N,   [param_N];
    mov.u32 %tid,  %tid.x;
    mov.u32 %ntid, %ntid.x;
    mov.u32 %bid,  %ctaid.x;
    mad.lo.u32 %idx, %bid, %ntid, %tid;
    setp.ge.u32 %p, %idx, %N;
    @%p bra done;
    cvt.u64.u32 %offset, %idx;
    mul.lo.u64  %offset, %offset, 8;
    add.u64 %a,   %a,   %offset;
    add.u64 %out, %out, %offset;
    ld.global.f64 %va, [%a];
    mov.f64 %zero, 0d0000000000000000;
    setp.lt.f64 %q, %va, %zero;
    selp.f64 %vc, %zero, %va, %q;
    st.global.f64 [%out], %vc;
done:
    ret;
}
)PTX";

static bool load_builtins()
{
    if (builtin_loaded) return true;
    if (!cuda_init()) return false;
    CUresult r = cuModuleLoadData(&builtin_module, BUILTIN_PTX_FULL);
    if (r != CUDA_SUCCESS)
    {
        const char* s = nullptr; cuGetErrorString(r, &s);
        set_error("load_builtins: %s", s ? s : "unknown");
        return false;
    }
    builtin_loaded = true;
    return true;
}

static bool run_builtin(const char* name, void** args, int n_args, int N, int block_size=256)
{
    if (!load_builtins()) return false;
    CUfunction fn;
    CU_CHECK_RET(cuModuleGetFunction(&fn, builtin_module, name), false);
    int grid = (N + block_size - 1) / block_size;
    CU_CHECK_RET(cuLaunchKernel(fn, grad, 1, 1, block_size, 1, 1, 0, nullptr, args, nullptr), false);
    return true;
}

bool cuda_builtin_add(int a, int b, int out)
{
    if (!cuda_tensor_to_device(a)) return false;
    if (!cuda_tensor_to_device(b)) return false;
    gpu_tensor_heap_ensure(out);
    int N = tensor_heap[a].rows * tensor_heap[a].cols;
    // Allocate output on device
    GPUTensor& go = gpu_tensor_heap[out];
    if (!go.on_device || go.bytes < (size_t)N*8)
    {
        if (go.on_device) cuMemFree(go.d_ptr);
        cuMemAlloc(&go.d_ptr, (size_t)N*8);
        go.bytes = (size_t)N*8;
        go.on_device = true;
    }
    go.rows = tensor_heap[a].rows;
    go.cols = tensor_heap[a].cols;
    void* args[] = { &gpu_tensor_heap[a].d_ptr, &gpu_tensor_heap[b].d_ptr, &go.d_ptr &N };
    return run_builtin("nova_vec_add", args, 4, N);
}

bool cuda_builtin_scale(int a, double s, int out)
{
    if (!cuda_tensor_to_device(a)) return false;
    gpu_tensor_heap_ensure(out);
    int N = tensor_heap[a].rows * tensor_heap[a].cols;
    GPUTensor& go = gpu_tensor_heap[out];
    if (!go.on_device || go.bytes < (size_t)N*8) {
        if (go.on_device) cuMemFree(go.d_ptr);
        cuMemAlloc(&go.d_ptr, (size_t)N*8);
        go.bytes = (size_t)N*8; go.on_device = true;
    }
    go.rows = tensor_heap[a].rows; go.cols = tensor_heap[a].cols;
    void* args[] = { &gpu_tensor_heap[a].d_ptr, &s, &go.d_ptr, &N };
    return run_builtin("nova_scale", args, 4, N);
}

bool cuda_builtin_relu(int a, int out)
{
    if (!cuda_tensor_to_device(a)) return false;
    gpu_tensor_heap_ensure(out);
    int N = tensor_heap[a].rows * tensor_heap[a].cols;
    GPUTensor& go = gpu_tensor_heap[out];
    if (!go.on_device || go.bytes < (size_t)N*8)
    {
        if (go.on_device) cuMemFree(go.d_ptr);
        cuMemAlloc(&go.d_ptr, (size_t)N*8);
        go.bytes = (size_t)N*8; go.on_device = true;
    }
    go.rows = tensor_heap[a].rows;
    go.cols = tensor_heap[a].cols;
    void* args[] = { &gpu_tensor_heap[a].d_ptr, &go.d_ptr, &N };
    return run_builtin("nova_relu", args, 3, N);
}

bool cuda_builtin_mul(int a, int b int out) 
{
    return cuda_builtin_add(a, b, out); /* placeholder */
}
bool cuda_builtin_sigmoid(int a, int out)
{
    return cuda_builtin_relu(a, out); /* placeholder */
}
bool cuda_builtin_matmul(int a, int b, int out) 
{
    return false; /*needs cuBLAS*/
}

// FFI dispatch

int ffi_cuda_load(const string& path)
{
    return cuda_load(path);
}
void ffi_cuda_to_device(int h)
{
    cuda_tensor_to_device(h);
}
void ffi_cuda_to_host(int h)
{
    cuda_tensor_to_host(h);
}
void ffi_cuda_free(int h)
{
    cuda_tensor_free(h);
}
void ffi_cuda_sync()
{
    cuda_sync();
}

int ffi_cuda_launch_packed(const int* arg_slots, int num_args)
{
    // Layout: [mod_handle, strMem_idx, gX, gY, gZ, bX, bY, bZ, tensor0, tensor1, ...]
    if (num_args < 0)
    {
        set_error("cuda_launch: need at least 8 args"); return -1;
    }
    int mod_handle = mem[arg_slots[0]];
    int str_idx = mem[arg_slots[1]];
    string func_name = strMem[str_idx];
    int gX = mem[arg_slots[2]], gY = mem[arg_slots[3]], gZ = mem[arg_slots[4]];
    int bX = mem[arg_slots[5]], bY = mem[arg_slots[6]], bZ = mem[arg_slots[7]];
    vector<int> handles;
    for (int i = 0;i < num_args;i++)
    {
        handles.push_back(mem[arg_slots[i]]);
    }
    cuda_launch(mod_handle, func_name, gX, gY, gZ, bX, bY, bZ, handles);
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
//  CUDA_AVAILABLE = 0  — CPU stub implementation (no CUDA SDK required)
// ══════════════════════════════════════════════════════════════════════════════
#else !CUDA_AVAILABLE

static void no_cuda(const char* fn)
{
    fprintf(stderr, "[CUDA FFI] '%s' called but NovaComp was built without CUDA "
                    "(recompile with -DNOVA_CUDA=1 and -lcuda -lcudart).\n", fn);
}

bool cuda_init()
{
    no_cuda("cuda_init");
    return false;
}
int cuda_load(const string& path)
{
    no_cuda("cuda_load"); return -1;
}
bool cuda_tensor_to_device(int h)
{
    no_cuda("cuda_tensor_to_device"); return false;
}
bool cuda_tensor_to_host(int h)       
{ 
    no_cuda("cuda_tensor_to_host"); return false; 

}
void cuda_tensor_free(int h)
{
    no_cuda("cuda_tensor_free");
}
void cuda_sync()
{
    no_cuda("cuda_sync");
}
bool cuda_builtin_add(int a, int b, int out)
{
    no_cuda("cuda_builtin_add"); return false;
}
bool cuda_builtin_mul(int a, int b, int out)
{
    no_cuda("cuda_builtin_mul"); return false;
}
bool cuda_builtin_scale(int a, double a, int out)
{
    no_cuda("cuda_builtin_scale"); return false;
}
bool cuda_builtin_relu(int a, int out)
{
    no_cuda("cuda_builtin_relu"); return false;
}
bool cuda_builtin_sigmoid(int a, int out)
{
    no_cuda("cuda_builtin_sigmoid"); return false;
}
bool cuda_builtin_matmul(int a, int b, int out)
{
    no_cuda("cuda_builtin_matmul"); return false;
}

bool cuda_launch(int, const string&, int, int, int, int, int, int,
                 const vector<int>&, bool)
{
    no_cuda("cuda_launch"); 
    return false;
}

int ffi_cuda_load(const string& p)
{
    no_cuda("cuda_load");
    return -1;
}
void ffi_cuda_to_device(int h)
{
    no_cuda("cuda_to_device");
}
void ffi_cuda_to_host(int h)
{
    no_cuda("cuda_to_host");
}
void ffi_cuda_free(int h)
{
    no_cuda("cuda_free");
}
void ffi_cuda_sync()
{
    no_cuda("cuda_sync");
}
int ffi_cuda_launch_packed(const int*, int)
{
    no_cuda("cuda_launch"); return -1;
}

#endif // CUDA_AVAILABLE;