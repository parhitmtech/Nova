/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Interpreter: I/O, int/float/double arithmetic, jumps, arrays, strings, casts. Split out of execute_program (interpreter.cc).
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <climits>
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

InstructionNode* exec_in(InstructionNode* pc)
{
    do {
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
                if (scanf("%d", &val) != 1) val = 0;  // no number (end of input or junk) reads as 0

                // capture for potential benchmark replay
                input_replay_buffer.push_back(val);
            }
            mem[pc->input_inst.var_index] = val;
            pc = pc->next;
            break;
    } while (0);
    return pc;
}

InstructionNode* exec_out(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}

InstructionNode* exec_assign(InstructionNode* pc)
{
    int op1 = 0, op2 = 0, result = 0;  // scratch, as in the original dispatch loop
    do {
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
                    // Both would otherwise kill the process with SIGFPE and no message.
                    if (op2 == 0)
                    {
                        fprintf(stderr, "Runtime error at line %d: integer division by zero\n", pc->line_no);
                        exit(1);
                    }
                    result = (op1 == INT_MIN && op2 == -1) ? INT_MIN : op1 / op2;  // wraps, like the hardware would
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
    } while (0);
    return pc;
}

InstructionNode* exec_cjmp(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}

InstructionNode* exec_jmp(InstructionNode* pc)
{
    do {
            if (pc->jmp_inst.target == NULL)
            {
                debug("Error: pc->jmp_inst->target is null.\n");
                exit(1);
            }
            pc = pc->jmp_inst.target;
            break;
    } while (0);
    return pc;
}

InstructionNode* exec_array_read(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}

InstructionNode* exec_array_write(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}

InstructionNode* exec_alloc(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}

InstructionNode* exec_strcat(InstructionNode* pc)
{
    do {
            int leftIdx = mem[pc->strcat_inst.left_slot];
            int rightIdx = mem[pc->strcat_inst.right_slot];
            strMem.push_back(strMem[leftIdx] + strMem[rightIdx]);
            int dest = next_str_available++;
            mem[pc->strcat_inst.dest_slot] = dest;
            pc = pc->next;
            break;
    } while (0);
    return pc;
}

InstructionNode* exec_scmp(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}

InstructionNode* exec_assign_f(InstructionNode* pc)
{
    do {
            float resultf = 0.0f;
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
    } while (0);
    return pc;
}

InstructionNode* exec_assign_d(InstructionNode* pc)
{
    do {
            // fprintf(stderr, "ASSIGN_D: left=%d op1=%d op=%d dmem.size=%d\n",
            //     pc->assign_d_inst.left_hand_side_index,
            //     pc->assign_d_inst.operand1_index,
            //     (int)pc->assign_d_inst.op,
            //     (int)dmem.size());
            double resultd = 0.0;
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
    } while (0);
    return pc;
}

InstructionNode* exec_cast(InstructionNode* pc)
{
    do {
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
    } while (0);
    return pc;
}
