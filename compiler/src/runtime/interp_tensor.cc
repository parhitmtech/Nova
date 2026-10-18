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

            switch (tc.op)
            {
                case TEN_ALLOC:
                {
                    res = alloc_tensor(mem[a[0]], mem[a[1]]);
                    break;
                }
                case TEN_ZEROS:
                {
                    res = ten_zeros({mem[a[0]], mem[a[1]]});
                    break;
                }
                case TEN_ONES:
                {
                    res = ten_ones({mem[a[0]], mem[a[1]]});
                    break;
                }
                case TEN_RANDN:
                {
                    res = ten_randn({mem[a[0]], mem[a[1]]});
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
                    double v = tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]);
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
                    tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]) = getScalar(3);
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
                    // Build entire output as single string then print once
                    string out_str = "";
                    out_str += "[";
                    for (int i = 0; i < T.rows; i++)
                    {
                        if (T.rows > 1) out_str += (i == 0 ? "[" : " [");
                        for (int j = 0; j < T.cols; j++)
                        {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%.4f", T.at(i, j));
                            out_str += buf;
                            if (j < T.cols - 1) out_str += ", ";
                        }
                        if (T.rows > 1) out_str += (i < T.rows - 1 ? "],\n" : "]");
                    }
                    out_str += "]\n";
                    printf("%s", out_str.c_str());
                    fflush(stdout);
                    res = 0;
                    break;
                }
                case TEN_MATMUL: 
                {
                    if (grad_mode)
                        res = ag_matmul(mem[a[0]], mem[a[1]]);
                    else
                        res = ten_matmul(mem[a[0]], mem[a[1]]); 
                    break;
                }
                case TEN_ADD: 
                {
                    if (grad_mode)
                        res = ag_add(mem[a[0]], mem[a[1]]);
                    else
                        res = ten_add(mem[a[0]], mem[a[1]]); 
                    break;
                }
                case TEN_SUB:
                { 
                    if (grad_mode)
                        res = ag_sub(mem[a[0]], mem[a[1]]);
                    else
                        res = ten_sub(mem[a[0]], mem[a[1]]); 
                    break;
                }
                case TEN_MUL: 
                {
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
                case TEN_T: 
                {
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
                case TEN_GET_ROW: res = ten_get_row(mem[a[0]], mem[a[1]]); break;
                case TEN_SET_ROW:
                {
                    ten_set_row(mem[a[0]], mem[a[1]], mem[a[2]]);
                    res = 0;
                    break;
                }
                case TEN_SLICE_ROWS: res = ten_slice_rows(mem[a[0]], mem[a[1]], mem[a[2]]); break;
                case TEN_RESHAPE:  // tensor_reshape(t, d0, d1, ...)
                {
                    std::vector<int> shape;
                    for (int i = 1; i < tc.num_args; i++) shape.push_back(mem[a[i]]);
                    res = ten_reshape(mem[a[0]], shape, pc->line_no);
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
