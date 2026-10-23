/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Interpreter: tensor built-ins (TENSOR_CALL) and compiler-inserted releases. Split out of execute_program (interpreter.cc).
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
#include "interp_internal.h"

using namespace std;

InstructionNode* exec_tensor_call(InstructionNode* pc)
{
    do {
            auto& tc = pc->tensor_call_inst;
            if (tc.op >= OBJ_NEW) return exec_object_call(pc);  // class instances
            if (tc.op >= CON_VEC_NEW) return exec_container_call(pc);  // vector / map
            int* a = tc.arg_slots;
            int res = 0;

            // Returns the scalar double value for arg i, regardless of whether it's
            // an int (mem[]), float (fmem[]), or double (dmem[]) arg.
            auto getScalar = [&](int i) -> double {
                switch (tc.arg_types[i]) {
                    case TYPE_FLOAT:  return (double)fmem[a[i]];
                    case TYPE_DOUBLE: return dmem[a[i]];
                    default:          return (double)mem[a[i]];
                }
            };

            // Every tensor argument must be a live handle. A Tensor declared without a value holds -1,
            // and a freed one is marked freed: either is a clear error here, not a read outside tensor_heap.
            {
                const char* sig = tensor_op_signature(tc.op);
                int len = (int)strlen(sig);
                bool repeat = (len > 0 && sig[len - 1] == '*');
                int fixed = repeat ? len - 1 : len;
                for (int i = 0; i < tc.num_args; i++)
                {
                    char want = (i < fixed) ? sig[i] : (repeat ? sig[fixed - 1] : 0);
                    if (want == 'T' && !tensor_is_live(mem[a[i]]))
                    {
                        fprintf(stderr, "Runtime error at line %d: tensor argument %d is not set or was already "
                                        "freed (handle %d)\n", pc->line_no, i + 1, mem[a[i]]);
                        exit(1);
                    }
                }
            }

            switch (tc.op)
            {
                case TEN_ALLOC:
                {
                    res = alloc_tensor(mem[a[0]], mem[a[1]]);
                    break;
                }
                case TEN_ZEROS:
                case TEN_ONES:
                case TEN_RANDN:
                {
                    // one argument per dimension: tensor_zeros(2, 3, 4) is 2x3x4
                    std::vector<int> shape;
                    for (int i = 0;i < tc.num_args; i++)
                    {
                        if (mem[a[i]] < 0)
                        {
                            tensor_shape_error(pc->line_no, "negative dimension " + std::to_string(mem[a[i]]));
                        }
                        shape.push_back(mem[a[i]]);
                    }
                    res = (tc.op == TEN_ZEROS) ? ten_zeros(shape)
                        : (tc.op == TEN_ONES) ? ten_ones(shape)
                        :                       ten_randn(shape);
                    break;
                }
                case TEN_XAVIER:
                {
                    res = ten_xavier(mem[a[0]], mem[a[1]]);
                    break;
                }
                case TEN_GET:
                {
                    // Returns a double: push to dmem and return the dslot index, same convention
                    // as TEN_SUM/TEN_MEAN/TEN_MAX below. Previously this truncated the value to int,
                    // silently dropping the fraction.
                    if (!tensor_is_live(mem[a[0]]))
                    {
                        fprintf(stderr, "Runtime error at line%d: tensor_get on a freed tensor handle (%d) - "
                                        "this means the compiler's tensor-free pass release it too early; "
                                        "pleae report this as bug.\n", pc->line_no, mem[a[0]]);
                        exit(1);
                    }
                    // double v = tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]);
                    // one index per dimension, checked against the shape
                    int idx[TENSOR_MAX_RANK + 1];
                    int n = tc.num_args - 1;
                    for (int k = 0;k < n && k <= TENSOR_MAX_RANK; k++) idx[k] = mem[a[k + 1]];
                    const Tensor& GT = tensor_heap[mem[a[0]]];
                    double v = GT.data[tensor_offset(GT, idx, n, "tensor_get", pc->line_no)];
                    int dslot = (int)dmem.size();
                    dmem.push_back(v);
                    next_double_available++;
                    res = dslot;
                    break;
                }
                case TEN_SET:
                {
                    if (!tensor_is_live(mem[a[0]]))
                    {
                        fprintf(stderr, "Runtime error at line %d: tensor_set on a freed handle (%d) - "
                                        "this means the compiler's tensor-free pass released it too early; "
                                        "please report this as a bug.\n", pc->line_no, mem[a[0]]);
                        exit(1);
                    }
                    // tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]) = getScalar(3);
                    // indices first, value last: tensor_set(t, i, j, k, v)
                    int idx[TENSOR_MAX_RANK + 1];
                    int n = tc.num_args - 2;
                    for (int k = 0; k < n && k <= TENSOR_MAX_RANK; k++) idx[k] = mem[a[k + 1]];
                    Tensor& ST = tensor_heap[mem[a[0]]];
                    ST.data[tensor_offset(ST, idx, n, "tensor_set", pc->line_no)] = getScalar(tc.num_args - 1);
                    res = 0;
                    break;
                }
                case TEN_ROWS:
                {
                    res = tensor_heap[mem[a[0]]].rows;
                    break;
                }
                case TEN_COLS:
                {
                    res = tensor_heap[mem[a[0]]].cols;
                    break;
                }
                case TEN_PRINT:
                {
                    Tensor& T = tensor_heap[mem[a[0]]];
                    // Build entire output as single string then print once.
                    // Rank 1 and 2 print exactly as before. A higher-rank tensor prints as a list
                    // of its 2-D blocks (the last two dimensions), separated by a blank line.
                    int bRows = (T.ndim() >= 2) ? T.shape[T.ndim() - 2] : 1;
                    int bCols = T.cols;
                    int nBlocks = (bRows * bCols == 0) ? 0 : (int)(T.data.size() / ((size_t)bRows * bCols));
                    string out_str = "";
                    if (T.ndim() > 2) out_str += "[";
                    for (int blk = 0; blk < nBlocks; blk++)
                    {
                        size_t base = (size_t)blk * bRows * bCols;
                        if (blk > 0) out_str += ",\n\n ";
                        out_str += "[";
                        for (int i = 0; i < bRows; i++)
                        {
                            if (bRows > 1) out_str += (i == 0 ? "[" : (T.ndim() > 2 ? "  [" : " ["));
                            for (int j = 0; j < bCols; j++)
                            {
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%.4f", T.data[base + (size_t)i * bCols + j]);
                                out_str += buf;
                                if (j < bCols - 1) out_str += ", ";
                            }
                            if (bRows > 1) out_str += (i < bRows - 1 ? "],\n" : "]");
                        }
                        out_str += "]";
                    }
                    if (T.ndim() > 2) out_str += "]";
                    out_str += "\n";
                    printf("%s", out_str.c_str());
                    fflush(stdout);
                    res = 0;
                    break;
                }
                case TEN_MATMUL: 
                {
                    // (..., n) @ (n, m): an N-D left side is treated as a stack of rows and keep its
                    // leading dimensions in the result. An N-D right side (batched matmul) comes in M1 stage 3.
                    require_2d(mem[a[1]], "matmul (right operand)", pc->line_no);
                    if (tensor_heap[mem[a[0]]].cols != tensor_heap[mem[a[1]]].rows)
                    {
                        tensor_shape_error(pc->line_no, "matmul: inner dimensions differ (" + 
                            std::to_string(tensor_heap[mem[a[0]]].cols) + " vs " + 
                            std::to_string(tensor_heap[mem[a[1]]].rows) + ")");
                    }
                    if (grad_mode)
                    {
                        res = ag_matmul(mem[a[0]], mem[a[1]]);
                    }
                    else
                    {
                        res = ten_matmul(mem[a[0]], mem[a[1]]);
                    }
                    if (tensor_heap[mem[a[0]]].ndim() > 2)
                    {
                        std::vector<int> outShape = tensor_heap[mem[a[0]]].shape;
                        outShape.back() = tensor_heap[mem[a[1]]].cols;
                        tensor_set_shape(tensor_heap[res], outShape);
                    }
                    break;
                }
                case TEN_ADD: 
                {
                    // + commutes, so a row on the left (b + x) moves to the right, the side that broadcasts
                    int x = mem[a[0]], y = mem[a[1]];
                    if (tensor_heap[x].rows == 1 && tensor_heap[y].rows > 1) std::swap(x, y);
                    require_elementwise(x, y, "+", pc->line_no, true);
                    if (grad_mode)
                        res = ag_add(x, y);
                    else
                        res = ten_add(x, y); 
                    break;
                }
                case TEN_SUB:
                { 
                    require_elementwise(mem[a[0]], mem[a[1]], "-", pc->line_no, true);
                    if (grad_mode)
                        res = ag_sub(mem[a[0]], mem[a[1]]);
                    else
                        res = ten_sub(mem[a[0]], mem[a[1]]); 
                    break;
                }
                case TEN_MUL: 
                {
                    require_elementwise(mem[a[0]], mem[a[1]], "*", pc->line_no, false);
                    if (grad_mode)
                        res = ag_mul(mem[a[0]], mem[a[1]]);
                    else
                        res = ten_mul(mem[a[0]], mem[a[1]]); 
                    break;
                }
                case TEN_SCALE: 
                {
                    if (grad_mode)
                        res = ag_scale(mem[a[0]], getScalar(1));
                    else
                        res = ten_scale(mem[a[0]], getScalar(1)); 
                    break;
                }
                case TEN_ADD_SCALAR:
                {
                    if (grad_mode)
                    {
                        res = ag_add_scalar(mem[a[0]], getScalar(1));
                    }
                    else
                    {
                        res = ten_add_scalar(mem[a[0]], getScalar(1));
                    }
                    break;
                }
                case TEN_DIV:
                {
                    require_elementwise(mem[a[0]], mem[a[1]], "/", pc->line_no, false);
                    if (grad_mode)
                    {
                        res = ag_div(mem[a[0]], mem[a[1]]);
                    }
                    else
                    {
                        res = ten_div(mem[a[0]], mem[a[1]]);
                    }
                    break;
                }
                case TEN_T: 
                {
                    require_2d(mem[a[0]], "tensor_T", pc->line_no);
                    if (grad_mode)
                        res = ag_transpose(mem[a[0]]);
                    else
                        res = ten_transpose(mem[a[0]]); 
                    break;
                }
                case TEN_SUM:
                {
                    double s = 0.0;
                    for (auto v : tensor_heap[mem[a[0]]].data)
                    {
                        s += v;
                    }
                    int dslot = (int)dmem.size();
                    dmem.push_back(s);
                    next_double_available++;
                    res = dslot;
                    break;
                }
                case TEN_MEAN:
                {
                    Tensor& T = tensor_heap[mem[a[0]]];
                    double s = 0.0;
                    for (auto v : T.data)
                    {
                        s += v;
                    }
                    int dslot = (int)dmem.size();
                    dmem.push_back(s / T.data.size());
                    next_double_available++;
                    res = dslot;
                    break;
                }
                case TEN_MAX:
                {
                    Tensor& T = tensor_heap[mem[a[0]]];
                    double mx = T.data[0];
                    for (auto v : T.data)
                    {
                        mx = std::max(mx, v);
                    }
                    int dslot = (int)dmem.size();
                    dmem.push_back(mx);
                    next_double_available++;
                    res = dslot;
                    break;
                }
                case TEN_CLIP: res = ten_clip(mem[a[0]], getScalar(1), getScalar(2)); break;
                case TEN_RELU:
                {
                    if (grad_mode)
                        res = ag_relu(mem[a[0]]);
                    else 
                        res = ten_relu(mem[a[0]]); 
                    break;
                }
                case TEN_SIGMOID: 
                {
                    if (grad_mode)
                        res = ag_sigmoid(mem[a[0]]);
                    else
                        res = ten_sigmoid(mem[a[0]]); 
                    break;
                }
                case TEN_TANH:
                { 
                    if (grad_mode)
                        res = ag_tanh_t(mem[a[0]]);
                    else
                        res = ten_tanh(mem[a[0]]); 
                    break;
                }
                case TEN_SOFTMAX: 
                {
                    if (grad_mode)
                        res = ag_softmax(mem[a[0]]);
                    else
                        res = ten_softmax(mem[a[0]]); 
                    break;
                }
                case TEN_RELU_GRAD: res = ten_relu_grad(mem[a[0]]); break;
                case TEN_SIGMOID_GRAD: res = ten_sigmoid_grad(mem[a[0]]); break;
                case TEN_MSE_LOSS:
                {
                    if (grad_mode)
                    {
                        res = ag_mse_loss(mem[a[0]], mem[a[1]]);
                    }
                    else
                    {
                        // Always return a 1x1 tensor handle, matching ag_mse_loss, 
                        // so mse_loss's result type doesn't depend on grad_mode.
                        // (Previously this branch returned a dmemdslot instead, which the parser
                        // typed identically to the tensor-handle brnahc - a real type mismatch 
                        // depending on runtime state.)
                        double loss = ten_mse_loss(mem[a[0]], mem[a[1]]);
                        int h= alloc_tensor(1, 1);
                        tensor_heap[h].at(0, 0) = loss;
                        res = h;
                    }
                    break;
                }
                case TEN_MSE_GRAD: res = ten_mse_grad(mem[a[0]], mem[a[1]]); break;
                case TEN_BCE_GRAD: res = ten_bce_grad(mem[a[0]], mem[a[1]]); break;
                case TEN_CE_LOSS:
                {
                    if (grad_mode)
                    {
                        res = ag_ce_loss(mem[a[0]], mem[a[1]]);
                    }
                    else
                    {
                        // same fix as ten_mse_loss, always a 1x1 tensor handle
                        double loss = ten_ce_loss(mem[a[0]], mem[a[1]]);
                        int h = alloc_tensor(1, 1);
                        tensor_heap[h].at(0, 0) = loss;
                        res = h;
                    }
                    break;
                }
                case TEN_CE_GRAD:    res = ten_ce_grad(mem[a[0]], mem[a[1]]); break;
                case TEN_SUM_AXIS0: 
                {
                    if (grad_mode)
                        res = ag_sum_axis0(mem[a[0]]);
                    else
                        res = ten_sum_axis0(mem[a[0]]); 
                    break;
                }
                case TEN_RAND_INT:
                {
                    res = ten_rand_int(mem[a[0]], mem[a[1]]);
                    break;
                }
                case TEN_GET_ROW:
                { 
                    require_2d(mem[a[0]], "tensor_get_row", pc->line_no);
                    res = ten_get_row(mem[a[0]], mem[a[1]]); 
                    break;
                }
                case TEN_SET_ROW:
                {
                    require_2d(mem[a[0]], "tensor_set_row", pc->line_no);
                    ten_set_row(mem[a[0]], mem[a[1]], mem[a[2]]);
                    res = 0;
                    break;
                }
                case TEN_SLICE_ROWS: 
                {
                    require_2d(mem[a[0]], "tensor_slice_rows", pc->line_no);
                    res = ten_slice_rows(mem[a[0]], mem[a[1]], mem[a[2]]); 
                    break;
                }
                case TEN_RESHAPE:  // tensor_reshape(t, d0, d1, ...)
                {
                    if (!tensor_is_live(mem[a[0]]))
                    {
                        tensor_shape_error(pc->line_no, "tensor_reshape on a freed or invalid tensor handle");
                    }
                    std::vector<int> shape;
                    for (int i = 1; i < tc.num_args; i++) shape.push_back(mem[a[i]]);
                    // ten_reshape checks the shape and fills in a -1; ag_reshape also records the gradient
                    res = grad_mode ? ag_reshape(mem[a[0]], shape, pc->line_no)
                                    : ten_reshape(mem[a[0]], shape, pc->line_no);
                    break;
                }
                case TEN_LOAD_CSV:
                {
                    int strIdx = mem[a[0]];
                    res = ten_load_csv(strMem[strIdx], mem[a[1]], mem[a[2]]);
                    if (res < 0) { fprintf(stderr, "tensor_load_csv failed\n"); exit(1); }
                    break;
                }
                // progress_bar(total, width) -> handle; progress_update(bar, step, loss); progress_done(bar)
                case TEN_PROGRESS_BAR:
                    res = ten_progress_bar(mem[a[0]], tc.num_args > 1 ? mem[a[1]] : 40);
                    break;
                case TEN_PROGRESS_UPDATE:
                    ten_progress_update(mem[a[0]], mem[a[1]], tc.num_args > 2 ? getScalar(2) : 0.0);
                    break;
                case TEN_PROGRESS_DONE:
                    ten_progress_done(mem[a[0]]);
                    break;
                case TEN_REQUIRES_GRAD:
                {
                    requires_grad(mem[a[0]]);
                    res = 0;
                    break;
                }
                case TEN_BACKWARD:
                {
                    backward(mem[a[0]]);
                    res = 0;
                    break;
                }
                case TEN_TENSOR_GRAD:
                {
                    res = tensor_grad(mem[a[0]]);
                    break;
                }
                case TEN_ZERO_GRAD:
                {
                    zero_grad(mem[a[0]]);
                    res = 0;
                    break;
                }
                case TEN_GRAD_STEP:
                {
                    res = grad_step(mem[a[0]], getScalar(1));
                    break;
                }
                case TEN_RANK:
                case TEN_DIM:
                {
                    int th = mem[a[0]];
                    if (!tensor_is_live(th))
                    {
                        fprintf(stderr, "Runtime error at line %d: %s on a freed or invalid tendor handle (%d)\n",
                                pc->line_no, tc.op == TEN_RANK ? "tensor_rank" : "tensor_dim", th);
                        exit(1);
                    }
                    Tensor& T = tensor_heap[th];
                    if (tc.op == TEN_RANK) { res = T.ndim(); break; }
                    int d = mem[a[1]];
                    if (d < 0 || d >= T.ndim())
                    {
                        fprintf(stderr, "Runtime error at line %d: tensor_dim: dimension %d out of range for a rank-%d tensor\n",
                                pc->line_no, d, T.ndim());
                        exit(1);
                    }
                    res = T.shape[d];
                    break;
                }
                case CON_VEC_NEW: case CON_MAP_NEW: case CON_PUSH: case CON_POP: case CON_POP_D: case CON_GET:
                case CON_GET_D: case CON_SET: case CON_SIZE: case CON_HAS: case CON_REMOVE: case CON_CLEAR:
                case CON_SORT: case CON_REVERSE: case CON_CONTAINS: case CON_INDEX_OF: case CON_BSEARCH: case CON_MIN: case CON_MIN_D: case CON_MAX: case CON_MAX_D: case CON_SUM: case CON_SUM_D:
                case OBJ_NEW: case OBJ_GET: case OBJ_GET_D: case OBJ_SET: case OBJ_FREE:
                    break;  // handled by exec_container_call above
                case TEN_LIVE_COUNT:
                {
                    // number of tensors not yet freed; lets a test assert "no leak"
                    int live = 0;
                    for (auto& t : tensor_heap) if (!t.freed) live++;
                    res = live;
                    break;
                }
            }
            if (tc.result_slot >= 0)
            {
                // TEN_SUM/TEN_MEAN/TEN_MAX/TEN_GET push their value to a freshdmem slot and leave 
                // its index in `res`; the parser allocated result_slot as a double slot 
                // (alloc_double_slot) for these ops, so copy the value across. Everything else stores
                // a tensor handle, int result directly in mem[] as before.
                switch (tc.op)
                {
                    case TEN_SUM: case TEN_MEAN: case TEN_MAX: case TEN_GET:
                    {
                        dmem[tc.result_slot] = dmem[res];
                        break;
                    }
                    default:
                    {
                        mem[tc.result_slot] = res;
                        break;
                    }
                }
            }
            pc = pc->next;
            break;
    } while (0);
    return pc;
}

InstructionNode* exec_tensor_release(InstructionNode* pc)
{
    do {
            // Compiler-inserted (see insert_tensor_releases()): this slot's tensor handle is dead 
            // from here on (per liveness) - release this alias. tensor_release() silently no-ops if mem[slot]
            // isn't a live handle; that should never happen given how insertion points are chosen 
            // (see the big comment above insert_tensor_releases()), but the no-op is kept as a defensive fallback rather
            // than a hard error, since a false positive here would halt an otherwise-correct program.
            tensor_release(mem[pc->tensor_release_inst.slot]);
            pc = pc->next;
            break;
    } while (0);
    return pc;
}
