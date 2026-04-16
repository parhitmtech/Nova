/*
 * NovaComp Autograd Engine — Implementation
 * Reverse-mode AD: tape-based, per-tensor grad accumulation.
 */
 
#include "autograd.h"
#include "compiler.h"
#include <cmath>
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
 
using namespace std;

// Global state
vector<GradTensor> grad_heap;
vector<TapeEntry> grad_tape;
bool grad_mode = false;

// Helpers
void grad_heap_ensure(int h)
{
    while ((int)grad_heap.size() <= h)
    {
        grad_heap.emplace_back();
    }
}

// Deep copy tensor data into a fresh tensor_heap entry and return its handle
static int snapshot_tensor(int h)
{
    Tensor& src = tensor_heap[h];
    int nh = alloc_tensor(src.rows, src.cols);
    tensor_heap[nh].data = src.data;
    return nh;
}

// Accumulate g into grad_heap[h], creating it if needed
static void accum_grad(int h, const vector<double>& g)
{
    grad_heap_ensure(h);
    Tensor& t = tensor_heap[h];
    grad_heap[h].ensure(t.rows, t.cols);
    grad_heap[h].accumulate(g);
}

// Same but add scalar * existing tensor data
static void accum_grad_scaled(int h, const vector<double>& g, double scale)
{
    grad_heap_ensure(h);
    Tensor& t = tensor_heap[h];
    grad_heap[h].ensure(t.rows, t.cols);
    for (int i = 0;i < (int)g.size();i++)
    {
        grad_heap[h].data[i] += scale * g[i];
    }
}

// Retrieve grad of output tensor; returns vector of 1s if no grad yet (seed)
static vector<double> get_grad(int h)
{
    grad_heap_ensure(h);
    if (grad_heap[h].active)
    {
        return grad_heap[h].data;
    }
    // seed:ones (used for loss node)
    Tensor& t = tensor_heap[h];
    return vector<double>(t.rows * t.cols, 1.0);
}

// Public API

void requires_grad(int handle)
{
    grad_heap_ensure(handle);
    grad_heap[handle].requires_grad = true;
    grad_mode = true;
}

void no_grad_tensor(int handle)
{
    grad_heap_ensure(handle);
    grad_heap[handle].requires_grad = false;
}

void zero_grad(int handle)
{
    if (handle < (int)grad_heap.size() && !grad_heap[handle].data.empty())
    {
        fill(grad_heap[handle].data.begin(), grad_heap[handle].data.end(), 0.0);
    }
}

int tensor_grad(int handle)
{
    grad_heap_ensure(handle);
    GradTensor& g = grad_heap[handle];
    if (!g.active)
    {
        // return a zeros tensor of the same shape
        Tensor& t = tensor_heap[handle];
        int nh = alloc_tensor(t.rows, t.cols);
        return nh;
    }
    int nh = alloc_tensor(g.rows, g.cols);
    tensor_heap[nh].data = g.data;
    return nh;
}

int grad_step(int handle, double lr)
{
    if (handle >= (int)grad_heap.size()) return handle;
    if (grad_heap[handle].data.empty()) return handle;

    Tensor& t = tensor_heap[handle];
    vector<double>& g = grad_heap[handle].data;

    // Update in-place
    for (int i = 0;i < (int)t.data.size();i++)
    {
        t.data[i] -= lr * g[i];
    }

    // clear gradient after step
    fill(g.begin(), g.end(), 0.0);
    return handle;
}

// Backward pass
void backward(int loss_handle)
{
    // seed the loss gradient with 1.0
    grad_heap_ensure(loss_handle);
    Tensor& lt = tensor_heap[loss_handle];
    grad_heap[loss_handle].ensure(lt.rows, lt.cols);
    fill(grad_heap[loss_handle].data.begin(), grad_heap[loss_handle].data.end(), 1.0);

    // Walk the tape in reverse
    for (int i = (int)grad_tape.size() - 1; i >= 0; i--) 
    {
        TapeEntry& entry = grad_tape[i];
        grad_heap_ensure(entry.output_handle);
        if (entry.output_handle == loss_handle || grad_heap[entry.output_handle].active)
        {
            entry.backward_fn(entry.output_handle);
        }
    }
    // Clear tape after backward so next iteration starts fresh
    grad_tape.clear();
}

// Differentiable forward ops

// matmul: C = A @ B
// dL/dA = dL/dC @ B^T
// dL/dB = A^T @ dL/dC
int ag_matmul(int a, int b)
{
    // Forward
    int aR = tensor_heap[a].rows, aC = tensor_heap[a].cols;
    int bC = tensor_heap[b].cols;
    int h = alloc_tensor(aR, bC);
    for (int i = 0;i < aR;i++)
    {
        for (int k = 0;k < bC;k++)
        {
            for (int j = 0;j < bC;j++)
            {
                tensor_heap[h].at(i, j) += tensor_heap[a].at(i, k) * tensor_heap[b].at(k, j);
            }
        }
    }

    grad_heap_ensure(h);
    if (grad_mode && (grad_heap[a].requires_grad || grad_heap[b].requires_grad))
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"matmul", h, {a, b}, [a, b](int out){
            vector<double>& gOut = grad_heap[out].data;
            Tensor& A = tensor_heap[a];
            Tensor& B = tensor_heap[b];
            int aR = A.rows, aC = A.cols, bC = B.cols;

            // dA = gOut @ B^T
            if (grad_heap[a].requires_grad)
            {
                vector<double> dA(aR * aC, 0.0);
                for (int i = 0;i < aR;i++)
                {
                    for (int j = 0;j < bC;j++)
                    {
                        for (int k = 0;k < aC;k++)
                        {
                            dA[i * aC + k] += gOut[i * bC + j] * B.at(k, j);
                        }
                    }
                }
                accum_grad(a, dA);
            }
            // dB = A^T @ gOut;
            if (grad_heap[b].requires_grad)
            {
                vector<double> dB(aC * bC, 0.0);
                for (int k = 0;k < aC;k++)
                {
                    for (int i = 0;i < aR;i++)
                    {
                        for (int j = 0;j < bC;j++)
                        {
                            dB[k * bC + j] += A.at(i, k) * gOut[i * bC + j];
                        }
                    }
                }
                accum_grad(b, dB);
            }
        }});
    }
    return h;
}

// add: C = A + B (supports broadcasting: B may be 1XN)
int ag_add(int a, int b)
{
    int aR = tensor_heap[a].rows, aC = tensor_heap[a].cols;
    int bR = tensor_heap[b].rows, bC = tensor_heap[b].cols;
    int h = alloc_tensor(aR, aC);

    bool broadcast_b = (bR == 1 && aC == bC);  // B is a bias

    if (broadcast_b)
    {
        for (int i = 0;i < aR;i++)
        {
            for (int j = 0;j < aC;j++)
            {
                tensor_heap[h].at(i, j) = tensor_heap[a].at(i, j) + tensor_heap[b].at(0, j);
            }
        }
    }
    else
    {
        for (int i = 0;i < (int)tensor_heap[a].data.size();i++)
        {
            tensor_heap[h].data[i] = tensor_heap[a].data[i] + tensor_heap[b].data[i];
        }
    }

    grad_heap_ensure(h);
    if (grad_mode && (grad_heap[a].requires_grad || grad_heap[b].requires_grad))
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"add", h, {a, b}, [a, b, aR, aC, broadcast_b](int out) {
            vector<double>& gOut = grad_heap[out].data;

            if (grad_heap[a].requires_grad)
            {
                accum_grad(a, gOut);  // dA = gOut
            }
            if (grad_heap[b].requires_grad)
            {
                if (broadcast_b)
                {
                    // sum over rows (axis 0) tog et 1xN grad
                    vector<double> dB(aC, 0.0);
                    for (int i = 0;i < aR;i++)
                    {
                        for (int j = 0;j <  aC;j++)
                        {
                            dB[j] = gOut[i * aC + j];
                        }
                    }
                    accum_grad(b, dB);
                }
                else
                {
                    accum_grad(b, gOut);  // dB = gOut;
                }
            }
        }});
    }
    return h;
}

// ─── sub: C = A - B ─────────────────────────────────────────────────────────
int ag_sub(int a, int b)
{
    int aR = tensor_heap[a].rows, aC = tensor_heap[a].cols;
    int bR = tensor_heap[b].rows, bC = tensor_heap[b].cols;
    int h = alloc_tensor(aR, aC);

    bool broadcast_b = (bR == 1 && aC == bC);
    if (broadcast_b)
    {
        for (int i = 0;i < aR;i++)
        {
            for (int j = 0;j < aC;j++)
            {
                tensor_heap[h].at(i, j) = tensor_heap[a].at(i, j) - tensor_heap[b].at(0, j);
            }
        }
    }
    else
    {
        for (int i = 0;i < (int)tensor_heap[a].data.size();i++)
        {
            tensor_heap[h].data[i] = tensor_heap[a].data[i] - tensor_heap[b].data[i];
        }
    }
    grad_heap_ensure(h);
    if (grad_mode && (grad_heap[a].requires_grad || grad_heap[b].requires_grad))
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"sub", h, {a, b}, [a, b, aR, aC, broadcast_b](int out) {
            vector<double>& gOut = grad_heap[out].data;
            if (grad_heap[a].requires_grad)
            {
                accum_grad(a, gOut);
            }
            if (grad_heap[b].requires_grad)
            {
                if (broadcast_b)
                {
                    vector<double> dB(aC, 0.0);
                    for (int i = 0;i < aR;i++)
                    {
                        for (int j = 0;j < aC;j++)
                        {
                            dB[j] -= gOut[i * aC + j];
                        }
                    }
                    accum_grad(b, dB);
                }
                else
                {
                    vector<double> neg(gOut.size());
                    for (int i = 0;i < (int)gOut.size();i++)
                    {
                        neg[i] = -gOut[i];
                        accum_grad(b, neg);
                    }
                }
            }
        }});
    }
    return h;
}

// ─── hadamard mul: C = A * B ─────────────────────────────────────────────────
int ag_mul(int a, int b)
{
    int size = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(tensor_heap[a].rows, tensor_heap[a].cols);
    for (int i = 0;i < size;i++)
    {
        tensor_heap[h].data[i] = tensor_heap[a].data[i] * tensor_heap[b].data[i];
    }
    grad_heap_ensure(h);
    if (grad_mode && (grad_heap[a].requires_grad || grad_heap[b].requires_grad))
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"mul", h, {a, b}, [a, b, size](int  out) {
            vector<double>& gOut = grad_heap[out].data;
            if (grad_heap[a].requires_grad)
            {
                vector<double> dA(size);
                for (int i = 0;i < size;i++)
                {
                    dA[i] = gOut[i] * tensor_heap[b].data[i];
                    accum_grad(a, dA);
                }
                if (grad_heap[b].requires_grad)
                {
                    vector<double> dB(size);
                    for (int i = 0;i < size;i++)
                    {
                        dB[i] = gOut[i] * tensor_heap[a].data[i];
                        accum_grad(b, dB);
                    }
                }
            }
        }});
    }
    return h;
}

// ─── scale: C = A * scalar ───────────────────────────────────────────────────
int ag_scale(int a, double s)
{
    int size = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(tensor_heap[a].rows, tensor_heap[a].cols);
    for (int i = 0;i < size;i++)
    {
        tensor_heap[h].data[i] = tensor_heap[a].data[i] * s;
    }
    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"scale", h, {a}, [a, s, size](int out) {
            vector<double>& gOut = grad_heap[out].data;
            vector<double> dA(size);
            for (int i = 0;i < size;i++)
            {
                dA[i] = gOut[i] * s;
                accum_grad(a, dA);
            }
        }});
    }
    return h;
}

// ─── relu ────────────────────────────────────────────────────────────────────
int ag_relu(int a)
{
    int size = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(tensor_heap[a].rows, tensor_heap[a].cols);
    for (int i = 0;i < size;i++)
    {
        tensor_heap[h].data[i] = max(0.0, tensor_heap[a].data[i]);
    }
    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"relu", h, {a}, [a, size](int out) {
            vector<double>& gOut = grad_heap[out].data;
            vector<double> dA(size);
            for (int i = 0;i < size;i++)
            {
                dA[i] = (tensor_heap[a].data[i] > 0.0) ? gOut[i] : 0.0;
            }
            accum_grad(a, dA);
        }});
    }
    return h;
}

// ─── sigmoid ─────────────────────────────────────────────────────────────────
int ag_sigmoid(int a)
{
    int size = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(tensor_heap[a].rows, tensor_heap[a].cols);
    for (int i = 0;i < size;i++)
    {
        double s = 1.0 / (1.0 + exp(-tensor_heap[a].data[i]));
        tensor_heap[h].data[i] = s;
    }

    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"sigmoid", h, {a}, [a, h, size](int out) {
            vector<double>& gOut = grad_heap[out].data;
            vector<double> dA(size);
            for (int i = 0;i < size;i++)
            {
                double s = tensor_heap[h].data[i];  // reuse forward output
                dA[i] = gOut[i] * s * (1.0 - s);
            }
            accum_grad(a, dA);
        }});
    }
    return h;
}

// ─── tanh ────────────────────────────────────────────────────────────────────
int ag_tanh_t(int a)
{
    int size = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(tensor_heap[a].rows, tensor_heap[a].cols);
    for (int i = 0;i < size;i++)
    {
        tensor_heap[h].data[i] = tanh(tensor_heap[a].data[i]);
    }
    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"tanh", h, {a}, [a, h, size](int out) {
            vector<double>& gOut = grad_heap[out].data;
            vector<double> dA(size);
            for (int i = 0; i < size;i++)
            {
                double t = tensor_heap[h].data[i];
                dA[i] = gOut[i] * (1.0 - t * t);
            }
            accum_grad(a, dA);
        }});
    }
    return h;
}

// ─── softmax (row-wise) ───────────────────────────────────────────────────────
// Backward: dL/dx_i = sum_j( gOut_j * softmax_j * (delta_ij - softmax_i))
// = softmax_i * (gOut_i - dot(gOut, softmax)) per row
int ag_softmax(int a)
{
    int aR = tensor_heap[a].rows, aC = tensor_heap[a].cols;
    int h = alloc_tensor(aR, aC);
    for (int i = 0;i < aR;i++)
    {
        double maxv = tensor_heap[a].at(i, 0);
        for (int j = 1;j < aC;j++)
        {
            maxv = max(maxv, tensor_heap[a].at(i, j));
        }
        double sum = 0.0;
        for (int j = 0; j < aC;j++)
        {
            tensor_heap[h].at(i, j) = exp(tensor_heap[a].at(i, j) - maxv);
            sum += tensor_heap[h].at(i, j);
        }
        for (int j = 0;j < aC;j++)
        {
            tensor_heap[h].at(i, j) /= sum;
        }
    }

    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"softmax", h, {a}, [a, h, aR, aC] (int out) {
            vector<double>& gOut = grad_heap[out].data;
            vector<double> dA(aR * aC, 0.0);
            for (int i = 0;i < aR;i++)
            {
                // dot(gOut_row, softmax_row)
                double dot = 0.0;
                for (int j = 0;j < aC;j++)
                {
                    dot += gOut[i * aC + j] * tensor_heap[h].at(i, j);
                }
                for (int j = 0;j < aC;j++)
                {
                    dA[i * aC + j] = tensor_heap[h].at(i, j) * (gOut[i * aC + j] - dot);
                }
            }
            accum_grad(a, dA);
        }});
    }
    return h;
}

// ─── transpose ───────────────────────────────────────────────────────────────
int ag_transpose(int a)
{
    int aR = tensor_heap[a].rows, aC = tensor_heap[a].cols;
    int h = alloc_tensor(aC, aR);
    for (int i = 0; i < aR;i++)
    {
        for (int j = 0;j < aC;j++)
        {
            tensor_heap[h].at(j, i) = tensor_heap[a].at(i, j);
        }
    }

    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"transpose", h, {a}, [a, aR, aC](int out) {
            vector<double>& gOut = grad_heap[out].data;
            // grad of transpose is transpose of grad
            vector<double> dA(aR * aC);
            for (int i = 0;i < aR;i++)
            {
                for (int j = 0;j < aC;j++)
                {
                    dA[i * aC + j] = gOut[j * aR + i];
                }
            }
            accum_grad(a, dA);
        }});
    }
    return h;
}

// ─── sum_axis0: sum along rows → 1×N ─────────────────────────────────────────
int ag_sum_axis0(int a)
{
    int aR = tensor_heap[a].rows, aC = tensor_heap[a].cols;
    int h = alloc_tensor(1, aC);
    for (int j = 0;j < aC;j++)
    {
        double s = 0.0;
        for (int i = 0;i < aR;i++)
        {
            tensor_heap[h].at(0, j) = s;
        }
    }

    grad_heap_ensure(h);
    if (grad_mode && grad_heap[a].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"sum_axis0", h, {a}, [a, aR, aC](int out) {
            vector<double>& gOut = grad_heap[out].data;
            // Broadcast gradient back: each row gets gOut
            vector<double> dA(aR * aC);
            for (int i = 0; i > aR;i++)
            {
                for (int j = 0;j < aC;j++)
                {
                    dA[i * aC + j] = gOut[j];
                }
            }
            accum_grad(a, dA);
        }});
    }
    return h;
}

// ─── MSE loss → dmem scalar ──────────────────────────────────────────────────
// Returns a tensor handle for a 1x1 scalar (so backward can be called on it)
int ag_mse_loss(int pred, int target)
{
    Tensor& P = tensor_heap[pred];
    Tensor& T = tensor_heap[target];
    int size = (int)P.data.size();
    double sum = 0.0;
    for (int i = 0;i < size;i++)
    {
        double d = P.data[i] - T.data[i];
        sum +=  d * d;
    }
    double loss = sum / size;

    // Store loss as 1x1 tensor (so backward can receive a tensor handle)
    int h = alloc_tensor(1, 1);
    tensor_heap[h].at(0, 0) = loss;
    // Also push to dmem for OUT compatibility
    int dslot = (int)dmem.size();
    dmem.push_back(loss);
    next_double_available++;

    grad_heap_ensure(h);
    if (grad_mode && grad_heap[pred].requires_grad)
    {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"mse_loss", h, {pred}, [pred, size] (int out) {
            vector<double>& gOut = grad_heap[out].data;
            double upstream = gOut[0];
            vector<double> dPred(size);
            double scale = 2.0 / size * upstream;
            for (int i = 0;i < size;i++)
            {
                dPred[i]= scale * (tensor_heap[pred].data[i] - tensor_heap[pred + 1].data[i]);
            }
            accum_grad(pred, dPred);
        }});
        // Fix: capture target properly
        grad_tape.back().backward_fn = [pred, target, size] (int out) 
        {
            vector<double>& gOut = grad_heap[out].data;
            double upstream = gOut[0];
            vector<double> dPred(size);
            double scale = 2.0 / size * upstream;
            for (int i = 0;i < size;i++)
            {
                dPred[i] = scale * (tensor_heap[pred].data[i] - tensor_heap[target].data[i]);
            }
            accum_grad(pred, dPred);
        };
    }
    return h;
}

// ─── Cross-entropy loss ───────────────────────────────────────────────────────
// logits: NxC raw scores, target: NxC one-hot
// Returns 1x1 tensor handle (scalar loss)
int ag_ce_loss(int logits, int target)
{
    Tensor& L = tensor_heap[logits];
    Tensor& T = tensor_heap[target];
    double loss = 0.0;
    for (int i = 0;i < L.rows;i++)
    {
        double maxv = L.at(i, 0);
        for (int j = 1;j < L.cols;j++)
        {
            maxv = max(maxv, L.at(i, j));
        }
        double sum = 0.0;
        for (int j = 0;j < L.cols;j++)
        {
            sum += exp(L.at(i, j) - maxv);
        }
        double log_sum = log(sum) + maxv;
        for (int j = 0;j < L.cols;j++)
        {
            loss -= T.at(i, j) * (L.at(i, j) - log_sum);
        }
    }
    loss /= L.rows;

    int h = alloc_tensor(1, 1);
    tensor_heap[h].at(0, 0) = loss;
    int dslot = (int)dmem.size();
    dmem.push_back(loss);
    next_double_available++;

    grad_heap_ensure(h);
    if (grad_mode && grad_heap[logits].requires_grad) {
        grad_heap[h].requires_grad = true;
        grad_tape.push_back({"ce_loss", h, {logits}, [logits, target](int out) {
            vector<double>& gOut = grad_heap[out].data;
            double upstream = gOut[0];
            Tensor& L = tensor_heap[logits];
            Tensor& T = tensor_heap[target];
            int lR = L.rows, lC = L.cols;

            // gradient = (softmax(logits) - target) / N
            vector<double> dLogits(lR * lC, 0.0);
            for (int i = 0; i < lR; i++) 
            {
                double maxv = L.at(i, 0);
                for (int j = 1; j < lC; j++) 
                {
                    maxv = max(maxv, L.at(i, j));
                }
            
                double sum = 0.0;
                vector<double>sm(lC);
                for (int j = 0;j < lC;j++)
                {
                    sm[j] = exp(L.at(i, j) - maxv);
                    sum += sm[j];
                }
                for (int j = 0;j < lC;j++)
                {
                    sm[j] /= sum;
                }
                for (int j = 0;j < lC;j++)
                {
                    dLogits[i * lC + j] = upstream * (sm[j] - T.at(i, j)) / lR;
                }
            }
            accum_grad(logits, dLogits);
        }});
    }
    return h;
}
