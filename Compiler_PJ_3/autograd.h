#pragma once
/*
 * NovaComp Autograd Engine
 * ────────────────────────────────────────────────────────────────────────────
 * Reverse-mode automatic differentiation (like PyTorch's autograd).
 *
 * Design:
 *   - Every Tensor in tensor_heap optionally carries a GradNode.
 *   - When requires_grad is set, every op that consumes that tensor records
 *     itself on a global tape (vector<TapeEntry>).
 *   - backward(handle) walks the tape in reverse, calls each op's backward
 *     lambda, and accumulates gradients into tensor.grad (a parallel tensor).
 *   - grad tensors live in grad_heap[], indexed the same as tensor_heap[].
 *
 * Nova language surface:
 *   requires_grad(t)          -- mark tensor as needing grad
 *   no_grad(t)                -- clear grad requirement
 *   backward(loss_handle)     -- run backprop from scalar loss
 *   tensor_grad(t)            -- return handle of gradient tensor
 *   zero_grad(t)              -- zero gradient of t
 *   grad_step(t, lr_dslot)    -- SGD step: t -= lr * t.grad (in-place new handle)
 * ────────────────────────────────────────────────────────────────────────────
 */
 
#ifndef NOVACOMP_AUTOGRAD_H
#define NOVACOMP_AUTOGRAD_H

#include <vector>
#include <functional>
#include <string>
#include <cstring>
#include <unordered_set>

// Forward Declaration
struct Tensor;
extern std::vector<Tensor> tensor_heap;
extern std::vector<double> dmem;
extern int next_double_available;
int alloc_tensor(int rows, int cols);

// Gradient storage
// grad_heap[i] mirrirs tensor_heap[i]
// If grad_heap[i].active == true, the tensor has an accumulated gradient.
struct GradTensor {
    bool active = false;  // true = gradient exists
    bool requires_grad = false;
    int rows, cols = 0;
    std::vector<double> data;  // same shape as the forward tensor

    void ensure(int r, int c)
    {
        if (!active)
        {
            rows = r;
            cols = c;
            data.assign(r * c, 0.0);
            active = true;
        }
    }

    void accumulate(const std::vector<double>& g)
    {
        for (int i = 0;i < (int)data.size();i++)
        {
            data[i] += g[i];
        }
    }
    double& at(int i, int j)
    {
        return data[i * cols + j];
    }
};

extern std::vector<GradTensor> grad_heap;

// Tape Entry

// Recorded for every differentiable op while grad_mode is active.
struct TapeEntry {
    std::string op_name;
    int output_handle;  // forward output tensor index
    std::vector<int> input_handles;  // forward input tensor indices
    // backward function: receives grad w.r.t output, accumulates into inputs
    std::function<void(int /*out handle*/)> backward_fn;
};

// Global tape
extern std::vector<TapeEntry> grad_tape;
extern bool grad_mode;  // true when inside a requires_grad

// API 

// Ensure a grad_heap is large enough to cover handle
void grad_heap_ensure(int h);

// Mark tensor as requiring gradient
void requires_grad(int handle);

// Clear gradient requirement
void no_grad_tensor(int handle);

// zero accumulated gradient for handle
void zero_grad(int handle);

// Run backward pass from a scalar loss tensor (shape 1x1 or any: s ends with 1s)
void backward(int loss_handle);

// Return the gradient tensor handle (allocates a new tensor from grad data)
// Callers get a read-only snapshot; the grad_heap entry is the live store
int tensor_grad(int handle);

// SGD-style in-place step: tensor[handle] -= lr * grad[handle]
// Returns a NEW tensor handle with updated weights.
int grad_step(int handle, double lr);

// Differentiable op wrappers 
// Each returns a tensor handle and (when grad_mode) puhes a TapeEntry
int ag_matmul(int a, int b);
int ag_add(int a, int b);
int ag_sub(int a, int b);
int ag_mul(int a, int b); // Hadamard
int ag_scale(int a, double s);
int ag_relu(int a);
int ag_sigmoid(int a);
int ag_tanh_t(int a);
int ag_softmax(int a);
int ag_transpose(int a);
int ag_sum_axis0(int a);           // sum along rows -> 1xN

// Loss functions - returns dmem index (double scalar), push tape
int ag_mse_loss(int pred, int target);  // returns dmem slot
int ag_ce_loss(int logits, int target);  // returns dmem slot

#endif  // NOVACOMP_AUTOGRAD_H