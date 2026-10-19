/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Tensor heap and tensor operations (the tensor_* built-ins). Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include <stack>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "lexer.h"
#include "compiler.h"
#include "bytecode.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"
#include "tensor_ops.h"
#include "progress_bar.h"

using namespace std;

// Tensor heap
std::vector<Tensor> tensor_heap;

void tensor_set_shape(Tensor& t, const std::vector<int>& shape)
{
    t.shape = shape;
    int n = (int)shape.size();
    t.strides.assign(n, 1);
    for (int i = n - 2; i >= 0; i--) t.strides[i] = t.strides[i+1] * shape[i + 1];
    //2-D view: last dimension is cols, everything before it folds into rows
    t.cols = (n == 0) ? 1 : shape[n - 1];
    t.rows = 1;
    for (int i = 0; i < n - 1; i++) t.rows *= shape[i];
}

int alloc_tensor_nd(const std::vector<int>& shape)
{
    Tensor t;
    tensor_set_shape(t, shape);
    size_t numel = 1;
    for (int d : shape) numel *= (size_t)d;
    t.data.assign(numel, 0.0);
    t.refCount = 1;  // owned by whichever mem[] slot recieves this handle
    t.freed = false;
    tensor_heap.push_back(std::move(t));
    return (int)tensor_heap.size() - 1;
}

int alloc_tensor(int rows, int cols)
{
    return alloc_tensor_nd({rows, cols});
}

int alloc_tensor_like(int src)
{
    // copy the shape first: allocating can move tensor_heap
    std::vector<int> shape = tensor_heap[src].shape;
    return alloc_tensor_nd(shape);
}

// Runtime shape errors stop the program with the line number, like the freed-handle errors do.
[[noreturn]] void tensor_shape_error(int line, const std::string& msg)
{
    fprintf(stderr, "Runtime error at line %d: %s\n", line, msg.c_str());
    exit(1);
}

// Flat offset of an N-D index, with rank and bounds checks.
size_t tensor_offset(const Tensor& T, const int* idx, int n, const char* op, int line)
{
    if (n != T.ndim())
    {
        tensor_shape_error(line, std::string(op) + ": a rank-" + std::to_string(T.ndim()) +
                           " tensor needs " + std::to_string(T.ndim()) + " indices, got " + std::to_string(n));
    }
    size_t off = 0;
    for (int k = 0; k < n; k++)
    {
        if (idx[k] < 0 || idx[k] >= T.shape[k])
        {
            tensor_shape_error(line, std::string(op) + ": index " + std::to_string(idx[k]) +
                               " out of range for dimension " + std::to_string(k) +
                               " (size " + std::to_string(T.shape[k]) + ")");
        }
        off += (size_t)idx[k] * T.strides[k];
    }
    return off;
}

// Ops that still work on the 2_D view only refuse N-D input instead of silently treating it as a 
// flattened matrix.
void require_2d(int h, const char* op, int line)
{
    if (tensor_heap[h].ndim() > 2)
    {
        tensor_shape_error(line, std::string(op) + " supports 1-D and 2-D tensors only (got rank " + 
                           std::to_string(tensor_heap[h].ndim()) + "); use tensor_reshape first");
    }
}

// Element-wise ops need equal shapes; + and - also accept an MxN matrix with a 1xN row on the right (a bias).
void require_elementwise(int a, int b, const char* op, int line, bool rowBroadcast)
{
    const Tensor& A = tensor_heap[a];
    const Tensor& B = tensor_heap[b];
    if (A.shape == B.shape) return;
    if (rowBroadcast && A.ndim() <= 2 && B.ndim() <= 2 && A.cols == B.cols && B.rows == 1) return;
    auto str = [](const Tensor& T) {
        std::string s = "(";
        for (size_t k = 0;k < T.shape.size();k++) s += (k ? ", " : "") + std::to_string(T.shape[k]);
        return s + ")";
    };
    tensor_shape_error(line, std::string("'") + op + "' needs tensors of the same shape, got " + str(A) + " and " + str(B));
}

// tensor_reshape(t, d0, d1, ...): a new tensor with the same elements in the same order. One dimension
// may be -1 and is inferred from the element count.
int ten_reshape(int a, std::vector<int> shape, int line)
{
    if (shape.empty() || (int)shape.size() > TENSOR_MAX_RANK)
        tensor_shape_error(line, "tensor_reshape needs 1 to " + std::to_string(TENSOR_MAX_RANK) + " dimensions");
    long long known = 1;
    int inferred = -1;
    for (int i = 0; i < (int)shape.size(); i++)
    {
        if (shape[i] == -1)
        {
            if (inferred >= 0) tensor_shape_error(line, "tensor_reshape: only one dimension can be -1");
            inferred = i;
        }
        else if (shape[i] <= 0)
            tensor_shape_error(line, "tensor_reshape: dimensions must be positive (or -1), got " +
                               std::to_string(shape[i]));
        else
            known *= shape[i];
    }
    long long count = (long long)tensor_heap[a].data.size();
    if (inferred >= 0)
    {
        if (count % known != 0)
            tensor_shape_error(line, "tensor_reshape: cannot infer the -1 dimension for " + std::to_string(count) + " elements");
        shape[inferred] = (int)(count / known);
        known = count;
    }
    if (known != count)
        tensor_shape_error(line, "tensor_reshape: new shape has " + std::to_string(known) +
                           " elements but the tensor has " + std::to_string(count));
    std::vector<double> data = tensor_heap[a].data;  // copy first: alloc may grow tensor_heap and move it
    int h = alloc_tensor_nd(shape);
    tensor_heap[h].data = std::move(data);  // same elements, same order
    return h;
}

bool tensor_is_live(int h)
{
    return h >= 0 && h < (int)tensor_heap.size() && !tensor_heap[h].freed;
}

// A second mem[] slot now also aliases this handle (plain reassignment, or a tensor passed into a function
// call as an argument). Must be paired with an eventual tensor_release() for that same new alias, or the tensor leaks.
void tensor_retain(int h)
{
    if (!tensor_is_live(h)) return;
    tensor_heap[h].refCount++;
}

// One fewer mem[] slot holds this handle (compiler-inserted, at the slot's last use per liveness 
// analysis). At refcoutn 0, the backing data is freed immediately; the tensor_heap entry itself is
// kept (index stability) but marked freed so any further access is a loud, detectable error instead
// of silent corruption.
void tensor_release(int h)
{
    if (!tensor_is_live(h)) return; // already freed, or not a tracked handle
    tensor_heap[h].refCount--;
    if (tensor_heap[h].refCount <= 0)
    {
        tensor_heap[h].data.clear();
        tensor_heap[h].data.shrink_to_fit();
        tensor_heap[h].shape.clear();
        tensor_heap[h].shape.shrink_to_fit();
        tensor_heap[h].strides.clear();
        tensor_heap[h].strides.shrink_to_fit();
        tensor_heap[h].freed = true;
        // Free its gradient buffer too, Otherwise every intermediate tensor of every 
        // training step keeps its gradient forever 
        if (h < (int)grad_heap.size())
        {
            grad_heap[h].data.clear();
            grad_heap[h].data.shrink_to_fit();
            grad_heap[h].active = false;
        }
    }
}

// Which mem[] object slots (instance fields, method self copies) currently own a reference to the
// tensor they hold. Only used for ASSIGNs with own_kind 1 or 2 (see compiler.h). Starts all false, so
// a field's initial 0 is never mistaken for an owned handle 0.
static std::vector<char> g_memOwns;
bool slot_owns(int i) { return i >= 0 && i < (int)g_memOwns.size() && g_memOwns[i]; }
void set_owns(int i, bool v)
{
    if (i < 0) return;
    if (i >= (int)g_memOwns.size()) g_memOwns.resize(i + 1, 0);
    g_memOwns[i] = v ? 1 : 0;
}

std::mt19937 rng(42);

struct ProgressBar {
    int total;
    int width;
};

static vector<ProgressBar> progress_heap;

// What each argument of a tensor built-in must be: 'T' a Tensor, 'n' a number (int, float, double
// or bool), 's' a string. A trailing '*' repeats the previous kind for any further arguments (one
// argument per dimension: tensor_zeros(2, 3, 4), tensor_get(t, i, j, k)).
const char* tensor_op_signature(TensorOp op)
{
    switch (op)
    {
        case TEN_ZEROS: case TEN_ONES: case TEN_RANDN:            return "n*";
        case TEN_ALLOC: case TEN_XAVIER: case TEN_RAND_INT:
        case TEN_PROGRESS_BAR:                                    return "nn";
        case TEN_GET: case TEN_SET: case TEN_RESHAPE:             return "Tn*";
        case TEN_MATMUL: case TEN_ADD: case TEN_SUB: case TEN_MUL: case TEN_DIV:
        case TEN_MSE_LOSS: case TEN_MSE_GRAD: case TEN_BCE_GRAD:
        case TEN_CE_LOSS: case TEN_CE_GRAD:                       return "TT";
        case TEN_SCALE: case TEN_GET_ROW: case TEN_GRAD_STEP: case TEN_ADD_SCALAR:
        case TEN_DIM:                                             return "Tn";
        case TEN_CLIP: case TEN_SLICE_ROWS:                       return "Tnn";
        case TEN_SET_ROW:                                         return "TnT";
        case TEN_LOAD_CSV:                                        return "snn";
        case TEN_PROGRESS_UPDATE:                                 return "nnn";
        case TEN_PROGRESS_DONE:                                   return "n";
        case TEN_LIVE_COUNT:                                      return "";
        default:                                                  return "T";  // one tensor
    }
}

int ten_zeros(const std::vector<int>& shape) { return alloc_tensor_nd(shape); }

int ten_ones(const std::vector<int>& shape)
{
    int h = alloc_tensor_nd(shape);
    for (auto& v : tensor_heap[h].data) v = 1.0;
    return h;
}

int ten_randn(const std::vector<int>& shape)
{
    std::normal_distribution<double> dist(0.0, 1.0);
    int h = alloc_tensor_nd(shape);
    for (auto& v : tensor_heap[h].data) v = dist(rng);
    return h;
}

int ten_rand_int(int lo, int hi)
{
    std::uniform_int_distribution<int> dist(lo, hi);
    int h = alloc_tensor(1, 1);
    tensor_heap[h].at(0, 0) = (double)dist(rng);
    return h;
}

int ten_xavier(int r, int c)
{
    double limit = std::sqrt(6.0 / (r + c));
    std::uniform_real_distribution<double> dist(-limit, limit);
    int h = alloc_tensor(r, c);
    for (auto& v : tensor_heap[h].data) v = dist(rng);
    return h;
}

int ten_matmul(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int bCols = tensor_heap[b].cols;
    int h = alloc_tensor(aRows, bCols);
    for (int i = 0; i < aRows; i++)
        for (int k = 0; k < aCols; k++)
            for (int j = 0; j < bCols; j++)
                tensor_heap[h].at(i, j) += tensor_heap[a].at(i, k) * tensor_heap[b].at(k, j);
    return h;
}

int ten_add(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int bRows = tensor_heap[b].rows, bCols = tensor_heap[b].cols;
    int h;
    if (aRows == bRows && aCols == bCols)
    {
        h = alloc_tensor_like(a);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] + tensor_heap[b].data[i];
    }
    else if (bRows == 1 && aCols == bCols)
    {
        h = alloc_tensor_like(a);
        for (int i = 0; i < aRows; i++)
            for (int j = 0; j < aCols; j++)
                tensor_heap[h].at(i, j) = tensor_heap[a].at(i, j) + tensor_heap[b].at(0, j);
    }
    else if (aRows == 1 && aCols == bCols)
    {
        h = alloc_tensor_like(b);
        for (int i = 0; i < bRows; i++)
            for (int j = 0; j < bCols; j++)
                tensor_heap[h].at(i, j) = tensor_heap[a].at(0, j) + tensor_heap[b].at(i, j);
    }
    else
    {
        h = alloc_tensor_like(a);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] + tensor_heap[b].data[i];
    }
    return h;
}

int ten_sub(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int bRows = tensor_heap[b].rows, bCols = tensor_heap[b].cols;
    int h;
    if (aRows == bRows && aCols == bCols)
    {
        h = alloc_tensor_like(a);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] - tensor_heap[b].data[i];
    }
    else if (bRows == 1 && aCols == bCols)
    {
        h = alloc_tensor_like(a);
        for (int i = 0; i < aRows; i++)
            for (int j = 0; j < aCols; j++)
                tensor_heap[h].at(i, j) = tensor_heap[a].at(i, j) - tensor_heap[b].at(0, j);
    }
    else
    {
        h = alloc_tensor_like(a);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] - tensor_heap[b].data[i];
    }
    return h;
}

int ten_sum_axis0(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int h = alloc_tensor(1, aCols);
    for (int j = 0; j < aCols; j++)
    {
        double s = 0.0;
        for (int i = 0; i < aRows; i++) s += tensor_heap[a].at(i, j);
        tensor_heap[h].at(0, j) = s;
    }
    return h;
}

int ten_mul(int a, int b)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = tensor_heap[a].data[i] * tensor_heap[b].data[i];
    return h;
}

int ten_scale(int a, double s)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = tensor_heap[a].data[i] * s;
    return h;
}

int ten_add_scalar(int a, double k)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
    {
        tensor_heap[h].data[i] = tensor_heap[a].data[i] + k;
    }
    return h;
}

int ten_div(int a, int b)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
    {
        tensor_heap[h].data[i] = tensor_heap[a].data[i] / tensor_heap[b].data[i];
    }
    return h;
}

int ten_transpose(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int h = alloc_tensor(aCols, aRows);
    for (int i = 0; i < aRows; i++)
        for (int j = 0; j < aCols; j++)
            tensor_heap[h].at(j, i) = tensor_heap[a].at(i, j);
    return h;
}

int ten_clip(int a, double lo, double hi)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = std::max(lo, std::min(hi, tensor_heap[a].data[i]));
    return h;
}

int ten_relu(int a)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = std::max(0.0, tensor_heap[a].data[i]);
    return h;
}

int ten_sigmoid(int a)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = 1.0 / (1.0 + std::exp(-tensor_heap[a].data[i]));
    return h;
}

int ten_tanh(int a)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = std::tanh(tensor_heap[a].data[i]);
    return h;
}

int ten_softmax(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int h = alloc_tensor_like(a);
    for (int i = 0; i < aRows; i++)
    {
        double maxv = tensor_heap[a].at(i, 0);
        for (int j = 1; j < aCols; j++) maxv = std::max(maxv, tensor_heap[a].at(i, j));
        double sum = 0.0;
        for (int j = 0; j < aCols; j++)
        {
            tensor_heap[h].at(i, j) = std::exp(tensor_heap[a].at(i, j) - maxv);
            sum += tensor_heap[h].at(i, j);
        }
        for (int j = 0; j < aCols; j++) tensor_heap[h].at(i, j) /= sum;
    }
    return h;
}

int ten_relu_grad(int a)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = tensor_heap[a].data[i] > 0.0 ? 1.0 : 0.0;
    return h;
}

int ten_sigmoid_grad(int a)
{
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor_like(a);
    for (int i = 0; i < sz; i++)
    {
        double s = 1.0 / (1.0 + std::exp(-tensor_heap[a].data[i]));
        tensor_heap[h].data[i] = s * (1.0 - s);
    }
    return h;
}

double ten_mse_loss(int pred, int target)
{
    Tensor& P = tensor_heap[pred];
    Tensor& T = tensor_heap[target];
    double sum = 0.0;
    for (int i = 0; i < (int)P.data.size(); i++)
    {
        double d = P.data[i] - T.data[i];
        sum += d * d;
    }
    return sum / P.data.size();
}

int ten_mse_grad(int pred, int target)
{
    int sz = (int)tensor_heap[pred].data.size();
    int h = alloc_tensor_like(pred);
    double scale = 2.0 / sz;
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = scale * (tensor_heap[pred].data[i] - tensor_heap[target].data[i]);
    return h;
}

int ten_bce_grad(int pred, int target)
{
    int size = (int)tensor_heap[pred].data.size();
    int h = alloc_tensor_like(pred);
    double scale = 1.0 / size;
    for (int i = 0;i < size;i++)
    {
        // gradient = (pred - Y) / N (after sigmoid cancellation)
        tensor_heap[h].data[i] = scale * (tensor_heap[pred].data[i] - tensor_heap[target].data[i]);
    }
    return h;
}

double ten_ce_loss(int logits, int target)
{
    Tensor& L = tensor_heap[logits];
    Tensor& T = tensor_heap[target];
    double loss = 0.0;
    for (int i = 0; i < L.rows; i++)
    {
        double maxv = L.at(i, 0);
        for (int j = 1; j < L.cols; j++) maxv = std::max(maxv, L.at(i, j));
        double sum = 0.0;
        for (int j = 0; j < L.cols; j++) sum += std::exp(L.at(i, j) - maxv);
        double log_sum = std::log(sum) + maxv;
        for (int j = 0; j < L.cols; j++) loss -= T.at(i, j) * (L.at(i, j) - log_sum);
    }
    return loss / L.rows;
}

int ten_ce_grad(int logits, int target)
{
    int lRows = tensor_heap[logits].rows, lCols = tensor_heap[logits].cols;
    int h = ten_softmax(logits);  // safe: reads logits by index inside, no held ref
    for (int i = 0; i < lRows; i++)
        for (int j = 0; j < lCols; j++)
            tensor_heap[h].at(i, j) = (tensor_heap[h].at(i, j) - tensor_heap[target].at(i, j)) / lRows;
    return h;
}

//Extract a single row as 1xN tensor
int ten_get_row(int a, int row)
{
    int cols = tensor_heap[a].cols;
    int h = alloc_tensor(1, cols);
    for (int j = 0;j < cols;j++)
    {
        tensor_heap[h].at(0, j) = tensor_heap[a].at(row, j);
    }
    return h;
}

// Copy src 1xN tensor into row of dst
void ten_set_row(int dst, int row, int src)
{
    int cols = tensor_heap[dst].cols;
    for (int j = 0;j < cols;j++)
    {
        tensor_heap[dst].at(row, j) = tensor_heap[src].at(0, j);
    }
}

// Copy rows [start, end) into a new tensor
int ten_slice_rows(int a, int start, int end)
{
    int cols = tensor_heap[a].cols;
    int h = alloc_tensor(end - start, cols);
    for (int i = start;i < end;i++)
    {
        for (int j = 0;j < cols;j++)
        {
            tensor_heap[h].at(i - start, j) = tensor_heap[a].at(i, j);
        }
    }
    return h;
}

int ten_progress_bar(int total, int width)
{
    return tqdm_init(total, width);
}

void ten_progress_update(int handle, int epoch, double loss)
{
    tqdm_update(handle, epoch, loss);
}

void ten_progress_done(int handle)
{
    tqdm_done(handle);
}

int ten_load_csv(const string& path, int rows, int cols)
{
    ifstream f(path);
    if (!f.is_open())
    {
        fprintf(stderr, "tensor_load_csv: cannot open '%s'\n", path.c_str());
        return -1;
    }
    int h = alloc_tensor(rows, cols);
    string line;
    int row = 0;
    while (getline(f, line) && row < rows)
    {
        istringstream ss(line);
        string token;
        int col = 0;
        while (getline(ss, token, ',') && col < cols)
        {
            try { tensor_heap[h].at(row, col) = stod(token); }
            catch (...) {}
            col++;
        }
        row++;
    }
    return h;
}
