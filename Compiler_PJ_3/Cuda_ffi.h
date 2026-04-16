#pragma once
/*
 * NovaComp CUDA FFI
 * ────────────────────────────────────────────────────────────────────────────
 * Allows Nova programs to call CUDA kernels via a simple FFI interface.
 *
 * Nova language surface syntax:
 *
 *   // Load a .ptx or .cubin kernel
 *   int mod = cuda_load("kernels/my_kernel.ptx") ;
 *
 *   // Launch a kernel: module, function name, grid, block, tensor args
 *   cuda_launch(mod, "vec_add", gridX, gridY, gridZ, blockX, blockY, blockZ,
 *               tensorA, tensorB, tensorOut) ;
 *
 *   // Synchronize device
 *   cuda_sync() ;
 *
 *   // Copy tensor from GPU to CPU (after kernel writes)
 *   cuda_tensor_to_host(tensorHandle) ;
 *
 *   // Copy tensor from CPU to GPU (before kernel reads)
 *   cuda_tensor_to_device(tensorHandle) ;
 *
 *   // Free GPU memory for tensor
 *   cuda_tensor_free(tensorHandle) ;
 *
 * Build: compile with -DNOVA_CUDA=1 and link against -lcuda -lcudart
 *        Without the flag, all calls are no-ops with error messages.
 * ────────────────────────────────────────────────────────────────────────────
 */

#ifndef NOVACOMP_CUDA_FFI_H
#define NOVACOMP_CUDA_FFI_H

#include <string>
#include <vector>
#include <unoredered_map>

// CUDA availability detection
#ifndef NOVA_CUDA 
#include <cuda.h>
#include <cuda_runtime.h>
#define CUDA_AVAILABLE 1
#else
#   define CUDA_AVAILABLE 0
    // Stub types so the rest of the code compiles without CUDA SDK
    typedef void* CUmodule;
    typedef void* CUfunction;
    typedef void* CUdeviceptr;
    typedef void* CUresult;
#   define CUDA_SUCCESS 0
#endif

// GPU tensor mirror
// Each tensor in tensor_heap[] may have a corressponding GPU allocation.
struct GPUTensor {
    bool on_device = false;  // true = device memory is allocated and current
    CUdeviceptr d_ptr = 0;
    size_t bytes = 0;
    int rows = 0;
    int cols = 0;
};

extern std::vector<GPUTensor> gpu_tensor_heap;  // parallel to tensor_heap

// Module registry
// cuda_load() returns an int handle into module registry
struct CUDAModule {
    std::string_path;
    CUmodule module;
    std::unoredered_map<std::string, Cufunction> func_cache;
};

extern std::vector<CUDAModule> module_registry;

// CUDA FFI API

// Initialize CUDA context (called once, lazily on first use)
bool cuda_init();

// Load a PTX/CUBIN file. Returns module handle (int index), or -1 on error
int cuda_load(const std::string& path);

// Ensure GPU tensor heap covers handle h
void gpu_tensor_heap_ensure(int h);

// Upload tensor_heap[h] -> GPU (allocates if needed, always uploads)
bool cuda_tensor_to_device(int handle);

// Download GPU -> tensor_heap[h] (no-op if not on device)
bool cuda_tensor_to_host(int handle);

// Free GPU allocation for handle
void cuda_tensor_free(int handle);

// Synchronize all CUDA operations
void cuda_sync();

// Kernel lanch
//
// Launch kernel from module[mod_handle] with function name func_name.
// grid / block dims: (gX, gY, gZ), (bX, bY, bZ)
// tensor_handles: list of tensor handles to pass as device pointers.
//                 Each handle must have been cuda_tensor_to_device'd first,
//                 OR pass cuda_auto_upload=true to upload automatically.
//
// Kernel signature must accept:  (double* t0, double* t1, ..., int rows, int cols)
// where rows/cols are from the FIRST tensor in tensor_handles.
//
bool cuda_launch(
    int mod_handle,
    const std::string& func_name,
    int gX, int gY, int gZ,
    int bX, int bY, int bZ,
    const std::vector<int>& tensor_handles,
    bool auto_upload = true;
)

// ── Nova TensorOp extensions for CUDA ────────────────────────────────────────
// These are registered as new TensorOps in compiler.h:
//   TEN_CUDA_LOAD, TEN_CUDA_LAUNCH, TEN_CUDA_SYNC,
//   TEN_CUDA_TO_DEVICE, TEN_CUDA_TO_HOST, TEN_CUDA_FREE
//
// The executor in compiler.cc calls these dispatch functions:

int ffi_cuda_load(const std::string& path);
void ffi_cuda_to_device(int handle);
void ffi_cuda_to_host(int handle);
void ffi_cuda_free(int handle);
void ffi_cuda_sync();

// Launch with packed args: arg_slots[0]=mod, arg_slots[1]=str_slot(func),
// arg_slots[2..7]=gridX,gridY,gridZ,blockX,blockY,blockZ,
// remaining = tensor handles
int ffi_cuda_launch_packed(const int* arg_slots, int num_args);

// ── Built-in CUDA kernels (shipped as inline PTX) ─────────────────────────────
// These are compiled at build time and available without loading external files.

// Element-wise ops on tensors already on GPU
bool cuda_builtin_add(int a, int b, int out);  // out = a + b;
bool cuda_builtin_mul(int a, int b, int out);  // out = a * b; (hadamard)
bool cuda_builtin_scale(int a, double s, int out);  // out = a * s;
bool cuda_builtin_relu(int a, int out);  // out = relu(a)
bool cuda_builtin_sigmoid(int a, int out);  // out = sigmoid(a);
bool cuda_builtin_matmul(int a, int b, int out);  // out = a @ b (naive CUDA)

// ── Error handling ─────────────────────────────────────────────────────────────
const char* cuda_ffi_last_error();
void cuda_ffi_clear_error();

#endif  // NOVACOMP_CUDA_FFI_H