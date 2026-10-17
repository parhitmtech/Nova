/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// The IR interpreter (execute_program). Split out of compiler.cc.
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

using namespace std;

// JIT-compile a function after this many interpreted calls (Windows builds only).
static const int JIT_THRESHOLD = 1;
// Remote directory on SOL for training jobs (relative paths resolve from the SOL home directory).
static string sol_work_dir = getenv("SOL_WORK_DIR") ? getenv("SOL_WORK_DIR") : "novacomp_jobs";

void execute_program(struct InstructionNode* program)
{
    struct InstructionNode* pc = program;
    int op1, op2, result;

    // Infinite loop detection:
    // Simple (non-tensor) instructions execute millions/sec in tight loops.
    // ML training with tensor ops executes only ~100K nodes for 500 epochs.
    // Limit: 500M simple instructions before aborting.
    // Each TENSOR_CALL grants +10M extra budget (rewards real ML work).
    static const long long SIMPLE_LIMIT = 500'000'000LL;
    static const long long TENSOR_BONUS =  10'000'000LL;
    long long simple_budget = SIMPLE_LIMIT;

    while (pc != NULL)
    {
        // Debug mode pause
        if (debug_mode && pc->line_no > 0 && pc->line_no > import_line_offset)
        {
            bool should_pause = debug_step;
            if (!should_pause && debug_breakpoints.count(pc->line_no - import_line_offset))
            {
                should_pause = true;
            }
            if (should_pause)
            {
                debug_print_state(pc->line_no - import_line_offset);
                string cmd = debug_wait_command();
                if (cmd == "stop")
                {
                    printf("__DEBUG__{\"type\":\"stopped\"}\n");
                    fflush(stdout);
                    return;
                }
                else if (cmd == "continue")
                {
                    debug_step = false;
                }
            }
        }
        // Decrement budget for every instruction.
        // TENSOR_CALL grants a large bonus so ML programs are never falsely killed.
        if (pc->type == TENSOR_CALL)
            simple_budget += TENSOR_BONUS;
        else
            simple_budget--;

        if (simple_budget <= 0)
        {
            fprintf(stderr,
                "\nError: execution limit reached (~500M instructions). "
                "Check for an infinite loop.\n"
                "Tip: ML training programs won't hit this limit — "
                "each tensor op grants extra budget.\n");
            exit(1);
        }

        switch (pc->type)  // main instruction dispatch switch
        {
            case NOOP:
                pc = pc->next;
                break;

            case IN:
            {
                int val = 0;
                if (input_replay_index >= 0)
                {
                    // ── replay mode (benchmark) ───────────────────────────────
                    if (input_replay_index < (int)input_replay_buffer.size())
                        val = input_replay_buffer[input_replay_index++];
                }
                else
                {
                    // ── live stdin mode ───────────────────────────────────────
                    scanf("%d", &val);

                    // capture for potential benchmark replay
                    input_replay_buffer.push_back(val);
                }
                mem[pc->input_inst.var_index] = val;
                pc = pc->next;
                break;
            }

            case OUT:
                if (!suppress_output)
                {
                    if (pc->output_inst.is_string)
                    {
                        // is_string_var: var_index is a mem slot holding a strMem index
                        // !is_string_var: var_index is a direct strMem index (literal)
                        int strIdx = pc->output_inst.is_string_var ? mem[pc->output_inst.var_index] : pc->output_inst.var_index;
                        printf("%s", strMem[strIdx].c_str());
                    }
                    else if (pc->output_inst.value_type == TYPE_FLOAT)
                    {
                        printf("%f", fmem[pc->output_inst.var_index]);
                    }
                    else if (pc->output_inst.value_type == TYPE_DOUBLE)
                    {
                        printf("%lf", dmem[pc->output_inst.var_index]);
                    }
                    else
                    {
                        printf("%d", mem[pc->output_inst.var_index]);
                    }
                    if (pc->output_inst.newline)
                        printf("\n");
                    else
                        printf(" ");
                    fflush(stdout);
                }
                pc = pc->next;
                break;

            case ASSIGN:
                switch (pc->assign_inst.op)
                {
                    case OPERATOR_PLUS:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 + op2;
                        break;
                    case OPERATOR_MINUS:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 - op2;
                        break;
                    case OPERATOR_MULT:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 * op2;
                        break;
                    case OPERATOR_DIV:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 / op2;
                        break;
                    case OPERATOR_NONE:
                        op1 = mem[pc->assign_inst.operand1_index];
                        result = op1;
                        break;
                }

                //mem[pc->assign_inst.left_hand_side_index] = result;
                
                {
                    int lhsIdx = pc->assign_inst.left_hand_side_index;
                    int srcIdx = pc->assign_inst.operand1_index;
                    int oldVal = mem[lhsIdx];
                    mem[lhsIdx] = result;
                    if (pc->assign_inst.op == OPERATOR_NONE && pc->assign_inst.own_kind == 1)
                    {
                        // Field store: the field gives up its old tensor and takes its own reference
                        // to the new one, if the value is a tensor. The source slot keeps its reference
                        // (and is released by liveness), so the field must retain. Retain before
                        // releasing so storing the same handle back into the field cannot free it.
                        bool isTensor = g_tensorSlots.count(srcIdx) > 0;
                        if (isTensor) tensor_retain(result);
                        if (slot_owns(lhsIdx)) tensor_release(oldVal);
                        set_owns(lhsIdx, isTensor);
                        pc = pc->next;
                        break;
                    }
                    if (pc->assign_inst.op == OPERATOR_NONE && pc->assign_inst.own_kind == 2)
                    {
                        // Field move (method copy-in / copy-back): the reference
                        // moves with the value; the refcount does not change
                        if (slot_owns(lhsIdx) && oldVal != result) tensor_release(oldVal);
                        set_owns(lhsIdx, slot_owns(srcIdx));
                        set_owns(srcIdx, false);
                        pc = pc->next;
                        break;
                    }
                }

                // A plain copy ("op == OPERATOR_NONE") of a tensor-tainted slot creates a second,
                // simultaneously-live alias of the same tensor_heap entry -- the source slot keeps
                // its value too, it isn't consumed. Without a retain here, the two aliases would share
                // one refcount, and the first of them to go dead (per the compiler's liveness-based TENSOR_RELEASE)
                // insertion) would free the handle out from under the other. An arithmetic op (+, -, *, /) never
                // applies to a tensor handle, so only OPERATOR_NONE needs this.
                if (pc->assign_inst.op == OPERATOR_NONE && g_tensorSlots.count(pc->assign_inst.left_hand_side_index))
                {
                    tensor_retain(mem[pc->assign_inst.left_hand_side_index]);
                }
                pc = pc->next;
                break;

            case CJMP:
                if (pc->cjmp_inst.target == NULL)
                {
                    debug("Error: pc->cjmp_inst->target is null.\n");
                    exit(1);
                }
                // float/double operands live in fmem[]/dmem[], not mem[]. This used to always read mem[]
                // regardless of type, so comparing a flaot/double variable in a condition silently comparing
                // the wrong memory (and for doubles, whose slot indices run past mem[]'s size, could read out of
                // bounds). operand_type is set by the parser per comparison.
                {
                    double d1, d2;
                    switch (pc->cjmp_inst.operand_type)
                    {
                        case TYPE_DOUBLE:
                        {
                            d1 = dmem[pc->cjmp_inst.operand1_index];
                            d2 = dmem[pc->cjmp_inst.operand2_index];
                            break;
                        }
                        case TYPE_FLOAT:
                        {
                            d1 = (double)fmem[pc->cjmp_inst.operand1_index];
                            d2 = (double)fmem[pc->cjmp_inst.operand2_index];
                            break;
                        }
                        default:
                        {
                            d1 = (double)mem[pc->cjmp_inst.operand1_index];
                            d2 = (double)mem[pc->cjmp_inst.operand2_index];
                            break;
                        }
                    }
                    switch (pc->cjmp_inst.condition_op)
                    {
                        case CONDITION_GREATER:
                        {
                            pc = (d1 > d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                        case CONDITION_LESS:
                        {
                            pc = (d1 < d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                        case CONDITION_NOTEQUAL:
                        {
                            pc = (d1 != d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                        case CONDITION_EQUAL:
                        {
                            pc = (d1 == d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                    }
                }
                break;

            case JMP:
                if (pc->jmp_inst.target == NULL)
                {
                    debug("Error: pc->jmp_inst->target is null.\n");
                    exit(1);
                }
                pc = pc->jmp_inst.target;
                break;

            case CALL:
            {
                if (pc->call_inst.function_head == NULL)
                {
                    debug("Error: CALL target function_head is null.\n");
                    exit(1);
                }

                // JIT hot path check
                InstructionNode* fhead = pc->call_inst.function_head;
                jit_call_counts[fhead]++;

                // Memoization fast path: pure + recursive functions cache arg→result
                bool is_memo = pure_functions.count(fhead) && recursive_functions.count(fhead);
                vector<int> memo_key;
                if (is_memo)
                {
                    for (int i = 0; i < pc->call_inst.num_params; i++)
                        memo_key.push_back(mem[pc->call_inst.arg_val_slots[i]]);
                    auto fit = memo_cache.find(fhead);
                    if (fit != memo_cache.end())
                    {
                        auto kit = fit->second.find(memo_key);
                        if (kit != fit->second.end())
                        {
                            const vector<int>& retVals = kit->second;
                            if (!retVals.empty())
                            {
                                mem[pc->call_inst.ret_val_index] = retVals[0];
                                if (pc->call_inst.all_ret_slots)
                                {
                                    for (int i = 0; i < (int)retVals.size() && i < pc->call_inst.num_ret_slots; i++)
                                        mem[pc->call_inst.all_ret_slots[i]] = retVals[i];
                                }
                            }
                            pc = pc->next;
                            break;
                        }
                    }
                }

                // Fast path: native JIT call (skip if previously deoptimized)
                if (!jit_deoptimized.count(fhead) && jit_compiled.count(fhead) && jit_compiled[fhead] != nullptr)
                {
                    typedef int64_t (*JitFunc)(int64_t);
                    JitFunc fn = (JitFunc)jit_compiled[fhead];
                    int argVal = mem[pc->call_inst.arg_val_slots[0]];
                    bool deopt = false;
                    int64_t result = 0;

                    #ifdef _MSC_VER
                    __try
                    {
                        result = fn((int64_t)argVal);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        deopt = true;
                    }
                    #else
                    result = fn((int64_t)argVal);
                    if (result == INT64_MIN)
                        deopt = true;
                    #endif

                    if (deopt)
                    {
                        fprintf(stderr, "JIT: deopt function, falling back to interpreter\n");
                        jit_deoptimized.insert(fhead);
                        jit_compiled[fhead] = nullptr;
                        // Fall through to interpreter path below - do NOT break
                    }
                    else
                    {
                        mem[pc->call_inst.ret_val_index] = (int)result;
                        pc = pc->next;
                        break;
                    }
                }
                else if (jit_call_counts[fhead] >= JIT_THRESHOLD && !jit_compiled.count(fhead)
                         && !jit_deoptimized.count(fhead)
                         && pc->call_inst.num_params <= 1)  // JIT only handles single-param functions
                {
#if defined(_WIN32) && !defined(NOVA_NO_JIT)  // -DNOVA_NO_JIT: always interpret
                    jit_compiled[fhead] = jit_compile_function(fhead);

                    if (!jit_compiled[fhead])
                    {
                        jit_compiled[fhead] = nullptr;
                    }
#endif
                }
                
                // Interpreter fallback path
                CallFrame frame;
                frame.returnAddress  = pc->next;
                frame.dest_index     = pc->call_inst.ret_val_index;
                frame.dest_type      = pc->call_inst.ret_type;
                frame.slot_base      = pc->call_inst.func_slot_base;
                frame.is_memoizable  = is_memo;
                frame.memo_func      = is_memo ? fhead : nullptr;
                frame.memo_key       = memo_key;

                if (pc->call_inst.all_ret_slots)
                {
                    frame.allRetSlots = vector<int>(
                        pc->call_inst.all_ret_slots,
                        pc->call_inst.all_ret_slots + pc->call_inst.num_ret_slots
                    );
                    if (pc->call_inst.ret_slot_types)
                    {
                        frame.allRetTypes = vector<VarType>(
                            pc->call_inst.ret_slot_types,
                            pc->call_inst.ret_slot_types + pc->call_inst.num_ret_slots
                        );
                    }
                }

                int base  = pc->call_inst.func_slot_base;
                int count = pc->call_inst.func_slot_count;
                for (int i = 0; i < count; i++)
                    frame.savedSlots.push_back(mem[base + i]);

                for (int i = 0; i < pc->call_inst.num_params; i++)
                {
                    int pSlot = pc->call_inst.param_slots[i];
                    int aSlot = pc->call_inst.arg_val_slots[i];
                    VarType pt = (pc->call_inst.param_types && i < pc->call_inst.num_params)
                                ? pc->call_inst.param_types[i] : TYPE_UNKNOWN;
                    VarType at = (pc->call_inst.arg_types) ? pc->call_inst.arg_types[i] : pt;
                    double val = 0.0;
                    if (at == TYPE_FLOAT)  val = (double)fmem[aSlot];
                    else if (at == TYPE_DOUBLE) val = dmem[aSlot];
                    else val = (double)mem[aSlot];
                    if (pt == TYPE_FLOAT) fmem[pSlot] = (float)val;
                    else if (pt == TYPE_DOUBLE) dmem[pSlot] = val;
                    else mem[pSlot]  = (int)val;
                    // Binding a tensor-handle argument into a param slot creates a second
                    // simultaneous alias (caller's aSLot, callee's pSlot) to the same tensor_heap 
                    // entry -- unlike RET, whc=ich is a transfer, not a duplicate. Retain here so the 
                    // two sides' independent TENSOR_RELEASE insertions (see insert_tensor_releases) each
                    // own one reference instead of double-freeing a single one.
                    if (g_tensorSlots.count(pSlot)) tensor_retain(mem[pSlot]);
                }

                callStack.push(frame);
                pc = pc->call_inst.function_head;
                break;
            }

            case RET:
            {
                if (callStack.empty())
                {
                    debug("Error: RET with empty call stack.\n");
                    exit(1);
                }
                CallFrame frame = callStack.top();
                callStack.pop();
                
                // capture return value before restoring slots
                // int ret_val = mem[pc->ret_inst.ret_val_index];

                // restore function-local slots to their pre-call  values
                // for (int i = 0; i < (int)frame.savedSlots.size(); i++)
                //     mem[frame.slot_base + i] = frame.savedSlots[i];

                // write return value - caller reads this in the very next ASSIGN
                // mem[frame.dest_index] = ret_val;

                // capture all return values before restore
                // vector<int> capturedRetVals;
                // for (int slot : frame.allRetSlots)
                // {
                //     capturedRetVals.push_back(mem[slot]);
                // }
                // if single return, fall back to ret_inst
                // if (capturedRetVals.empty())
                // {
                //     capturedRetVals.push_back(mem[pc->ret_inst.ret_val_index]);
                // }

                // Populate memo cache for pure recursive functions on first computation
                // if (frame.is_memoizable && frame.memo_func)
                // {
                //     memo_cache[frame.memo_func][frame.memo_key] = capturedRetVals;
                // }

                // restore function-local slots
                // for (int i = 0;i < (int)frame.savedSlots.size();i++)
                // {
                //     mem[frame.slot_base + i] = frame.savedSlots[i];
                // }

                // write all return values back
                // for (int i = 0;i < (int)capturedRetVals.size() && i < (int)frame.allRetSlots.size();i++)
                // {
                //     mem[frame.allRetSlots[i]] = capturedRetVals[i];
                // }
                // //backward compatibility - also to dst_index
                // mem[frame.dest_index] = capturedRetVals[0];

                // Return slots can liev in mem[], fmem[], or dmem[]. Read and write each through 
                // its own array. Old code assumed mem[] for all. double is the common carry type
                // for all three
                vector<double> capturedRetVals;
                for (int i = 0;i < (int)frame.allRetSlots.size();i++)
                {
                    int slot = frame.allRetSlots[i];
                    VarType rt = (i < (int)frame.allRetTypes.size()) ? frame.allRetTypes[i] : TYPE_UNKNOWN;
                    double v = (rt == TYPE_FLOAT) ? (double)fmem[slot] :
                                (rt == TYPE_DOUBLE) ? dmem[slot] : (double)mem[slot];
                    capturedRetVals.push_back(v);
                }
                // single return: ues the RET node's own slot and type
                if (capturedRetVals.empty())
                {
                    int slot = pc->ret_inst.ret_val_index;
                    VarType rt = pc->ret_inst.ret_type;
                    double v = (rt == TYPE_FLOAT) ? (double)fmem[slot] :
                               (rt == TYPE_DOUBLE) ? dmem[slot] : (double)mem[slot];
                    capturedRetVals.push_back(v);
                }

                // memo cache stays int-only (safe: edit 2 keeps non-int functions out)
                if (frame.is_memoizable && frame.memo_func)
                {
                    vector<int> intRetVals;
                    for (double v : capturedRetVals) intRetVals.push_back((int)v);
                    memo_cache[frame.memo_func][frame.memo_key] = intRetVals;
                }

                // restore function-local slots (mem[] only, unchanged)
                for (int i = 0;i < (int)frame.savedSlots.size();i++)
                {
                    mem[frame.slot_base + i] = frame.savedSlots[i];
                }

                // write all return values back, each through its own array
                for (int i = 0;i < (int)capturedRetVals.size() && i < (int)frame.allRetSlots.size();i++)
                {
                    int slot = frame.allRetSlots[i];
                    VarType rt = (i < (int)frame.allRetTypes.size()) ? frame.allRetTypes[i] : TYPE_UNKNOWN;
                    if (rt == TYPE_FLOAT) fmem[slot] = (float)capturedRetVals[i];
                    else if (rt == TYPE_DOUBLE) dmem[slot] = capturedRetVals[i];
                    else mem[slot] = (int)capturedRetVals[i];
                }

                // also write dest_index, through its own array
                if (frame.dest_type == TYPE_FLOAT) fmem[frame.dest_index] = (float)capturedRetVals[0];
                else if (frame.dest_type == TYPE_DOUBLE) dmem[frame.dest_index] = capturedRetVals[0];
                else mem[frame.dest_index] = (int)capturedRetVals[0];

                // free all function-local slots EXCEPT dest_index
                // dest_index must survive until the callers ASSIGN copies to it
                // that ASSIGN emits no alloc_slot calls, so dest_index is dafe
                int count = (int)frame.savedSlots.size();
                for (int i = 0;i < count;i++)
                {
                    int slot = frame.slot_base + i;
                    if (slot != frame.dest_index)
                    {
                        free_slot(slot);
                    }
                }
                pc = frame.returnAddress;
                break;
            }

            case ARRAY_READ:
            {
                int base = pc->array_inst.dynamic_base
                           ? mem[pc->array_inst.base_index]
                           : pc->array_inst.base_index;
                int size = (pc->array_inst.size_slot >= 0)
                           ? mem[pc->array_inst.size_slot]
                           : pc->array_inst.array_size;
                int idx  = mem[pc->array_inst.index_slot];
                if (idx < 0 || idx >= size)
                {
                    printf("Runtime error at line %d: array index %d out of bounds (size %d)\n", pc->array_inst.line_no, idx, size);
                    exit(1);
                }
                mem[pc->array_inst.target_index] = mem[base + idx];
                pc = pc->next;
                break;
            }

            case ARRAY_WRITE:
            {
                int base = pc->array_inst.dynamic_base
                           ? mem[pc->array_inst.base_index]
                           : pc->array_inst.base_index;
                int size = (pc->array_inst.size_slot >= 0)
                           ? mem[pc->array_inst.size_slot]
                           : pc->array_inst.array_size;
                int idx  = mem[pc->array_inst.index_slot];
                if (idx < 0 || idx >= size)
                {
                    printf("Runtime error at line %d: array index %d out of bounds (size %d)\n",
                           pc->array_inst.line_no, idx, pc->array_inst.array_size);
                    exit(1);
                }
                mem[base + idx] = mem[pc->array_inst.target_index];
                pc = pc->next;
                break;
            }

            case ALLOC:
            {
                int size = mem[pc->alloc_inst.size_slot];
                if (size <= 0)
                {
                    printf("Runtime error: array size must > 0, got %d\n", size);
                    exit(1);
                }
                int base = next_available;
                for (int i = 0;i < size;i++)
                {
                    alloc_slot();
                }
                mem[pc->alloc_inst.base_slot] = base;
                pc = pc->next;
                break;
            }

            case STRCAT:
            {
                int leftIdx = mem[pc->strcat_inst.left_slot];
                int rightIdx = mem[pc->strcat_inst.right_slot];
                strMem.push_back(strMem[leftIdx] + strMem[rightIdx]);
                int dest = next_str_available++;
                mem[pc->strcat_inst.dest_slot] = dest;
                pc = pc->next;
                break;
            }

            case SCMP:
            {
                int leftIdx = mem[pc->scmp_inst.operand1_index];
                int rightIdx = mem[pc->scmp_inst.operand2_index];
                string& left = strMem[leftIdx];
                string& right = strMem[rightIdx];
                bool pass = false;
                switch (pc->scmp_inst.condition_op)
                {
                    case CONDITION_GREATER: pass = (left > right); break;
                    case CONDITION_LESS: pass = (left < right); break;
                    case CONDITION_NOTEQUAL: pass = (left != right); break;
                    case CONDITION_EQUAL: pass = (left == right); break;
                }
                pc = pass ? pc->next : pc->scmp_inst.target;
                break;
            }

            case ASSIGN_F:
            {
                float op1f, op2f, resultf;
                switch (pc->assign_f_inst.op)
                {
                    case OPERATOR_PLUS: 
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] + fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MINUS:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] - fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MULT:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] * fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_DIV:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] / fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_NONE:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index];
                        break;
                    }
                }
                fmem[pc->assign_f_inst.left_hand_side_index] = resultf;
                pc = pc->next;
                break;
            }

            case ASSIGN_D:
            {
                // fprintf(stderr, "ASSIGN_D: left=%d op1=%d op=%d dmem.size=%d\n",
                //     pc->assign_d_inst.left_hand_side_index,
                //     pc->assign_d_inst.operand1_index,
                //     (int)pc->assign_d_inst.op,
                //     (int)dmem.size());
                double op1d, op2d, resultd;
                switch (pc->assign_d_inst.op)
                {
                    case OPERATOR_PLUS:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] + dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MINUS:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] - dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MULT:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] * dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_DIV:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] / dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_NONE:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index];
                        break;
                    }
                } 
                dmem[pc->assign_d_inst.left_hand_side_index] = resultd;
                pc = pc->next;
                break;
            }

            case CAST:
            {
                int si = pc->cast_inst.src_index;
                int di = pc->cast_inst.dst_index;
                VarType st = pc->cast_inst.src_type;
                VarType dt = pc->cast_inst.dst_type;

                double val = 0.0;
                if (st == TYPE_INT) val = (double)mem[si];
                else if (st == TYPE_FLOAT) val = (float)fmem[si];
                else if (st == TYPE_DOUBLE) val = (double)dmem[si];

                if (dt == TYPE_INT) mem[di] = (int)val;
                else if (dt == TYPE_FLOAT) fmem[di] = (float)val;
                else if (dt == TYPE_DOUBLE) dmem[di] = (double)val;

                pc = pc->next;
                break;
            }

            case TENSOR_CALL:
            {
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
                    case TEN_LOAD_CSV:
                    {
                        int strIdx = mem[a[0]];
                        res = ten_load_csv(strMem[strIdx], mem[a[1]], mem[a[2]]);
                        if (res < 0) { fprintf(stderr, "tensor_load_csv failed\n"); exit(1); }
                        break;
                    }
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
            }

            case TENSOR_RELEASE:
            {
                // Compiler-inserted (see insert_tensor_releases()): this slot's tensor handle is dead 
                // from here on (per liveness) - release this alias. tensor_release() silently no-ops if mem[slot]
                // isn't a live handle; that should never happen given how insertion points are chosen 
                // (see the big comment above insert_tensor_releases()), but the no-op is kept as a defensive fallback rather
                // than a hard error, since a false positive here would halt an otherwise-correct program.
                tensor_release(mem[pc->tensor_release_inst.slot]);
                pc = pc->next;
                break;
            }

            case HF_INFER:
            {
                auto& hf = pc->hf_inst;
                string model = strMem[mem[hf.model_slot]];

                if (hf.op == HF_OP_SET_TOKEN) {
                    hf_api_token = model;
                    pc = pc->next;
                    break;
                }

                if (hf.op == HF_OP_DATASET) {
                    // model_slot holds dataset name, input_slot holds split string
                    string dsName  = model;
                    string dsSplit = (hf.input_slot >= 0) ? strMem[mem[hf.input_slot]] : "train";
                    string handle  = dsName + ":" + dsSplit;
                    int strIdx = (int)strMem.size();
                    strMem.push_back(handle);
                    if (hf.result_slot >= 0) mem[hf.result_slot] = strIdx;
                    pc = pc->next;
                    break;
                }

                // Trim any whitespace/newlines the parser may have left in the model name
                while (!model.empty() && (model.back() == ' ' || model.back() == '\n' ||
                       model.back() == '\r' || model.back() == '\t')) model.pop_back();
                while (!model.empty() && (model.front() == ' ' || model.front() == '\n' ||
                       model.front() == '\r' || model.front() == '\t')) model = model.substr(1);

                string tok = hf_api_token;
                if (tok.empty()) {
                    const char* env = getenv("HF_API_TOKEN");
                    if (env) tok = env;
                }
                // Trim token too
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\n' ||
                       tok.back() == '\r' || tok.back() == '\t')) tok.pop_back();

                string input = (hf.input_slot >= 0) ? strMem[mem[hf.input_slot]] : "";

                string payload;
                if (hf.op == HF_OP_GENERATE) {
                    payload = "{\"inputs\":\"" + hf_escape_json(input) +
                              "\",\"parameters\":{\"max_new_tokens\":" +
                              to_string(hf.max_tokens) + "}}";
                } else {
                    payload = "{\"inputs\":\"" + hf_escape_json(input) + "\"}";
                }

                string jobTag      = to_string((long long)(intptr_t)pc);
                string payloadFile = "/tmp/hf_payload_" + jobTag + ".json";
                string tmpOut      = "/tmp/hf_resp_"    + jobTag + ".json";
                string pyFile      = "/tmp/hf_req_"     + jobTag + ".py";

                // Write the JSON payload to a file — avoids all shell-quoting issues.
                { ofstream pf(payloadFile); pf << payload; }

                // Use Python urllib with:
                //   ProxyHandler({})  — disables all env-based proxies
                //   NoRedirect        — catches HTTP redirects before they reach HTML pages.
                //                       HF redirects requests with invalid/missing tokens
                //                       to huggingface.co (HTML) instead of returning JSON.
                string pyScript =
                    "import urllib.request, urllib.error, json, sys\n"
                    "\n"
                    "class NoRedirect(urllib.request.HTTPRedirectHandler):\n"
                    "    def redirect_request(self, req, fp, code, msg, headers, newurl):\n"
                    "        raise Exception('HF API redirected ' + str(code) + ' to: ' + newurl[:80]\n"
                    "                        + ' — check your HF token is valid')\n"
                    "\n"
                    "with open('" + payloadFile + "', 'rb') as f: data = f.read()\n"
                    "url = 'https://api-inference.huggingface.co/models/" + model + "'\n"
                    "req = urllib.request.Request(url, data=data, method='POST')\n"
                    "req.add_header('Authorization', 'Bearer " + tok + "')\n"
                    "req.add_header('Content-Type', 'application/json')\n"
                    "opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect)\n"
                    "try:\n"
                    "    with opener.open(req, timeout=30) as r:\n"
                    "        sys.stdout.write(r.read().decode('utf-8'))\n"
                    "except urllib.error.HTTPError as e:\n"
                    "    try: sys.stdout.write(e.read().decode('utf-8'))\n"
                    "    except: sys.stdout.write(json.dumps({'error': 'HTTP ' + str(e.code)}))\n"
                    "except Exception as e:\n"
                    "    sys.stdout.write(json.dumps({'error': str(e)}))\n";

                { ofstream pf(pyFile); pf << pyScript; }

                string pyCmd = "python3 " + pyFile + " > " + tmpOut + " 2>/dev/null";
                fprintf(stderr, "HF_CMD: python3 %s\n", pyFile.c_str());
                system(pyCmd.c_str());

                string response;
                {
                    ifstream rf(tmpOut);
                    response.assign(istreambuf_iterator<char>(rf),
                                    istreambuf_iterator<char>());
                }
                fprintf(stderr, "HF_RESP: %s\n", response.substr(0, 200).c_str());
                remove(payloadFile.c_str());
                remove(pyFile.c_str());
                remove(tmpOut.c_str());

                if (hf.result_slot >= 0) {
                    string text = hf_extract_text(response);
                    int strIdx = (int)strMem.size();
                    strMem.push_back(text);
                    mem[hf.result_slot] = strIdx;
                }

                pc = pc->next;
                break;
            }

            case SOL_CALL:
            {
                auto& si = pc->sol_inst;

                switch (si.op)
                {
                    case SOL_OP_SET_KEY:
                    {
                        // sol.set_key() is disabled — SSH key is configured server-side
                        // via SOL_PRIVATE_KEY_PATH to prevent users from accessing SOL directly.
                        break;
                    }

                    case SOL_OP_FINETUNE:
                    {
                        string model   = strMem[mem[si.model_slot]];
                        string task    = strMem[mem[si.task_slot]];
                        string dataset = (si.dataset_slot >= 0) ? strMem[mem[si.dataset_slot]] : "imdb:train";

                        // Parse "name:split" handle produced by hf.dataset()
                        string dsName = dataset, dsSplit = "train";
                        size_t sep = dataset.find(':');
                        if (sep != string::npos) {
                            dsName  = dataset.substr(0, sep);
                            dsSplit = dataset.substr(sep + 1);
                        }

                        string jobId  = to_string((long long)time(nullptr)) + "_" +
                                        to_string((unsigned)rand() % 10000);
                        string jobDir = sol_work_dir + "/" + jobId;

                        string trainPy = generateTrainScript(model, task, dsName, dsSplit,
                                                             si.epochs, si.lr, si.batch_size,
                                                             jobDir, hf_api_token);
                        string slurmSh = generateSlurmScript(jobId, jobDir);

                        string localPy    = "/tmp/train_" + jobId + ".py";
                        string localSlurm = "/tmp/job_"   + jobId + ".slurm";
                        sol_write_file(localPy,    trainPy);
                        sol_write_file(localSlurm, slurmSh);

                        sshExecSOL("mkdir -p " + jobDir);
                        sftpUploadSOL(localPy,    jobDir + "/train.py");
                        sftpUploadSOL(localSlurm, jobDir + "/job.slurm");

                        string sbatchOut  = sshExecSOL("sbatch " + jobDir + "/job.slurm");
                        string slurmJobId = sol_extract_slurm_id(sbatchOut);

                        if (slurmJobId.empty()) {
                            // sbatch failed or returned unexpected output — surface it to the user
                            printf("Nova: SOL sbatch failed. Output: %s\n",
                                   sbatchOut.empty() ? "(empty — tunnel may be down)" : sbatchOut.c_str());
                            // Use sentinel so the wait loop exits immediately
                            slurmJobId = "0";
                        }

                        printf("Nova: SOL finetune submitted — SLURM job ID %s\n", slurmJobId.c_str());

                        // Handle: "jobId:slurmJobId:jobDir:model"
                        string handle = jobId + ":" + slurmJobId + ":" + jobDir + ":" + model;
                        int strIdx = (int)strMem.size();
                        strMem.push_back(handle);
                        mem[si.job_slot] = strIdx;

                        remove(localPy.c_str());
                        remove(localSlurm.c_str());
                        break;
                    }

                    case SOL_OP_WAIT:
                    {
                        string handle = strMem[mem[si.job_slot]];
                        // Parse handle: "jobId:slurmJobId:jobDir:model"
                        size_t c1 = handle.find(':');
                        size_t c2 = (c1 != string::npos) ? handle.find(':', c1 + 1) : string::npos;
                        size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                        if (c1 == string::npos) { pc = pc->next; break; }
                        string slurmJobId = handle.substr(c1 + 1, c2 - c1 - 1);

                        if (slurmJobId == "0") {
                            printf("Nova: Skipping wait — job was not submitted.\n");
                            pc = pc->next;
                            break;
                        }

                        string jobDir_wait = (c3 != string::npos)
                            ? handle.substr(c2 + 1, c3 - c2 - 1)
                            : handle.substr(c2 + 1);

                        bool seenRunning = false;
                        int  runPolls    = 0;
                        string lastSnippet;

                        while (true) {
                            string status = sol_trim(sshExecSOL(
                                "squeue -j " + slurmJobId + " -h -o \"%T\" 2>/dev/null || echo DONE"));

                            if (status.empty() || status == "DONE" || status == "COMPLETED") {
                                printf("Nova: Training complete!\n");
                                break;
                            } else if (status == "FAILED" || status == "CANCELLED") {
                                fprintf(stderr, "Nova: Job %s %s on SOL cluster.\n",
                                        slurmJobId.c_str(), status.c_str());
                                break;
                            } else if (status == "PENDING" || status == "CONFIGURING") {
                                printf("Nova: Training job in queue, waiting for GPU server...\n");
                            } else if (status == "RUNNING") {
                                if (!seenRunning) {
                                    printf("Nova: GPU server acquired! Training started.\n");
                                    seenRunning = true;
                                    runPolls = 0;
                                }
                                // Stream a live snippet of the training log every poll (60s)
                                if (runPolls % 1 == 0) {
                                    string snippet = sol_trim(sshExecSOL(
                                        "tail -2 " + jobDir_wait + "/slurm_output.txt 2>/dev/null"));
                                    if (!snippet.empty() && snippet != lastSnippet) {
                                        printf("%s\n", snippet.c_str());
                                        lastSnippet = snippet;
                                    }
                                }
                                runPolls++;
                            }
#ifndef _WIN32
                            sleep(60);
#endif
                        }

                        // Show final training summary (metrics + upload confirmation)
                        string slurmLog = sol_trim(sshExecSOL(
                            "grep -v 'it/s\\]' " + jobDir_wait +
                            "/slurm_output.txt 2>/dev/null | tail -15"));
                        if (!slurmLog.empty())
                            printf("Nova: [Training summary]\n%s\n", slurmLog.c_str());

                        // Show any errors from SLURM stderr
                        string slurmErr = sol_trim(sshExecSOL(
                            "tail -10 " + jobDir_wait + "/slurm_error.txt 2>/dev/null"));

                        break;
                    }

                    case SOL_OP_PREDICT:
                    {
                        string handle = strMem[mem[si.job_slot]];
                        size_t c2 = handle.find(':', handle.find(':') + 1);
                        size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                        string jobDir = (c3 != string::npos)
                            ? handle.substr(c2 + 1, c3 - c2 - 1)
                            : handle.substr(c2 + 1);
                        string input  = strMem[mem[si.input_slot]];

                        // Escape input for embedding in Python string literal
                        string escaped;
                        for (char c : input) {
                            if (c == '"') escaped += "\\\"";
                            else if (c == '\\') escaped += "\\\\";
                            else escaped += c;
                        }

                        string inferPy =
                            "import sys, os, json\n"
                            "job_dir = '" + jobDir + "'\n"
                            "if not os.path.isdir(job_dir):\n"
                            "    print(f'[SOL predict] Job directory not found: {job_dir}')\n"
                            "    sys.exit(0)\n"
                            "if 'config.json' not in os.listdir(job_dir):\n"
                            "    print('[SOL predict] config.json missing — training likely failed')\n"
                            "    sys.exit(0)\n"
                            "try:\n"
                            "    import torch\n"
                            "    from transformers import AutoTokenizer, AutoModelForSequenceClassification\n"
                            "    tokenizer = AutoTokenizer.from_pretrained(job_dir, local_files_only=True)\n"
                            "    model = AutoModelForSequenceClassification.from_pretrained(\n"
                            "        job_dir, local_files_only=True, ignore_mismatched_sizes=True)\n"
                            "    model.eval()\n"
                            "    inputs = tokenizer(\"" + escaped + "\", return_tensors='pt',\n"
                            "                       truncation=True, padding=True, max_length=512)\n"
                            "    inputs.pop('token_type_ids', None)  # not all models use this\n"
                            "    with torch.no_grad():\n"
                            "        logits = model(**inputs).logits\n"
                            "    probs = torch.softmax(logits, dim=-1)[0]\n"
                            "    pred  = int(torch.argmax(probs).item())\n"
                            "    label = model.config.id2label.get(pred, f'LABEL_{pred}')\n"
                            "    score = float(probs[pred].item())\n"
                            "    print(f'{label} ({score:.3f})')\n"
                            "except Exception as e:\n"
                            "    print(f'[SOL predict error: {e}]')\n";

                        string localInfer  = "/tmp/infer_" + to_string((long long)(intptr_t)pc) + ".py";
                        string remoteInfer = jobDir + "/infer.py";
                        sol_write_file(localInfer, inferPy);
                        sftpUploadSOL(localInfer, remoteInfer);
                        remove(localInfer.c_str());

                        // Use bash -l (login shell) so module system init scripts are sourced.
                        // Direct SSH exec doesn't run /etc/profile.d/modules.sh, so
                        // `module load` would silently fail in a plain exec session.
                        string output = sol_trim(sshExecSOL(
                            "bash -l -c '"
                            "source /etc/profile.d/modules.sh 2>/dev/null || true; "
                            "module load python/3.10 2>/dev/null || true; "
                            "python3 " + remoteInfer + " 2>&1'"));
                        if (output.empty())
                            output = "[SOL predict: no output — model may not have saved correctly]";
                        int strIdx = (int)strMem.size();
                        strMem.push_back(output);
                        if (si.result_slot >= 0) mem[si.result_slot] = strIdx;
                        break;
                    }

                    case SOL_OP_SAVE:
                    {
                        string handle = strMem[mem[si.job_slot]];
                        size_t c1 = handle.find(':');
                        size_t c2 = (c1 != string::npos) ? handle.find(':', c1 + 1) : string::npos;
                        size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                        string jobId  = handle.substr(0, c1);
                        string jobDir = (c3 != string::npos) ? handle.substr(c2 + 1, c3 - c2 - 1) : handle.substr(c2 + 1);
                        string model  = (c3 != string::npos) ? handle.substr(c3 + 1) : "";
                        string savePath = strMem[mem[si.path_slot]];

                        // Check if the SLURM job uploaded the model to S3
                        string uploadedS3Key;
                        string slurmFullLog = sol_trim(sshExecSOL(
                            "cat " + jobDir + "/slurm_output.txt 2>/dev/null"));
                        const string UPLOAD_MARKER = "NOVA_MODEL_UPLOADED:";
                        size_t mpos = slurmFullLog.find(UPLOAD_MARKER);
                        if (mpos != string::npos) {
                            size_t end = slurmFullLog.find('\n', mpos + UPLOAD_MARKER.size());
                            uploadedS3Key = sol_trim(slurmFullLog.substr(
                                mpos + UPLOAD_MARKER.size(),
                                end == string::npos ? string::npos
                                                    : end - mpos - UPLOAD_MARKER.size()));
                        }

                        if (!uploadedS3Key.empty()) {
                            // Model was uploaded to S3 by the SLURM compute node
                            string fname = uploadedS3Key;
                            size_t sl = fname.rfind('/');
                            if (sl != string::npos) fname = fname.substr(sl + 1);
                            printf("Nova: Model saved to your file storage as '%s'\n", fname.c_str());
                            // Marker for server to register in DynamoDB (no upload needed)
                            printf("NOVA_S3_FILE:%s\n", uploadedS3Key.c_str());
                        } else {
                            // Fallback: write a small JSON reference — model stays on SOL scratch
                            string meta =
                                "{\n"
                                "  \"type\": \"hf_model_ref\",\n"
                                "  \"model\": \"" + model + "\",\n"
                                "  \"job_id\": \"" + jobId + "\",\n"
                                "  \"sol_path\": \"" + jobDir + "\"\n"
                                "}\n";

                            ofstream nbc(savePath);
                            nbc << meta;
                            nbc.close();

                            printf("Nova: Model reference saved to %s\n", savePath.c_str());
                            printf("Nova: Full model weights remain on SOL at %s\n", jobDir.c_str());
                            printf("NOVA_SAVE_FILE:%s\n", savePath.c_str());
                        }
                        break;
                    }
                }

                pc = pc->next;
                break;
            }

            default:
                debug("Error: invalid value for pc->type (%d).\n", pc->type);
                exit(1);
                break;
        }
    }
}
