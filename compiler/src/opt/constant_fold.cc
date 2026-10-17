/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Optimizer: constant folding, self-copy removal, algebraic simplification. Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "lexer.h"
#include "compiler.h"
#include "optimizer.h"

using namespace std;

// Constant folding 

static vector<bool> constant_slots;

void mark_constants(struct InstructionNode* program)
{
    int n = next_available;
    vector<int> write_count(n, 0);
    constant_slots.assign(n, true);
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN)
            write_count[pc->assign_inst.left_hand_side_index]++;
        if (pc->type == IN)
            write_count[pc->input_inst.var_index]++;
        if (pc->type == ARRAY_READ)
            write_count[pc->array_inst.target_index]++;
        pc = pc->next;
    }

    for (int i = 0;i < n;i++)
    {
        if (write_count[i] > 1) constant_slots[i] = false;
    }

    pc = program;
    while (pc != nullptr)
    {
        if (pc->type == IN)
            constant_slots[pc->input_inst.var_index] = false;
        if (pc->type == ARRAY_READ)
            constant_slots[pc->array_inst.target_index] = false;
        if (pc->type == ASSIGN && pc->assign_inst.op == OPERATOR_NONE)
            if (!constant_slots[pc->assign_inst.operand1_index])
                constant_slots[pc->assign_inst.left_hand_side_index] = false;
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
            if (!constant_slots[pc->assign_inst.operand1_index] ||
                !constant_slots[pc->assign_inst.operand2_index])
                constant_slots[pc->assign_inst.left_hand_side_index] = false;
        pc = pc->next;
    }
}

int fold_pass(struct InstructionNode* program)
{
    int folds = 0;
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
        {
            int op1_idx = pc->assign_inst.operand1_index;
            int op2_idx = pc->assign_inst.operand2_index;
            int dst_idx = pc->assign_inst.left_hand_side_index;

            if (constant_slots[op1_idx] && constant_slots[op2_idx])
            {
                int op1 = mem[op1_idx], op2 = mem[op2_idx], result = 0;
                switch (pc->assign_inst.op)
                {
                    case OPERATOR_PLUS:  result = op1 + op2; break;
                    case OPERATOR_MINUS: result = op1 - op2; break;
                    case OPERATOR_MULT:  result = op1 * op2; break;
                    case OPERATOR_DIV:
                        if (op2 == 0) { pc = pc->next; continue; }
                        result = op1 / op2; break;
                    default: pc = pc->next; continue;
                }
                mem[dst_idx]                   = result;
                pc->assign_inst.op             = OPERATOR_NONE;
                pc->assign_inst.operand1_index = dst_idx;
                constant_slots[dst_idx]        = true;
                folds++;
            }
        }
        pc = pc->next;
    }
    return folds;
}

int constant_fold(struct InstructionNode* program)
{
    mark_constants(program);
    int total_folds = 0, folds = 0;
    do {
        folds = fold_pass(program);
        total_folds += folds;
    } while (folds > 0);
    return total_folds;
}

int remove_self_copies(struct InstructionNode* head)
{
    int removed = 0;
    struct InstructionNode* pc = head;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN &&
            pc->assign_inst.op == OPERATOR_NONE &&
            pc->assign_inst.left_hand_side_index == pc->assign_inst.operand1_index)
        {
            pc->type = NOOP;
            removed++;
        }
        pc = pc->next;
    }
    return removed;
}

int algebraic_simplify(struct InstructionNode* program)
{
    int simplified = 0;
    int n = (int)constant_slots.size();

    // pre-allocate a zero slot for zero-product results
    int zero_slot = alloc_slot();
    mem[zero_slot] = 0;

    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
        {
            int op1 = pc->assign_inst.operand1_index;
            int op2 = pc->assign_inst.operand2_index;
            int dst = pc->assign_inst.left_hand_side_index;

            bool op1_const = (op1 < n && constant_slots[op1]);
            bool op2_const = (op2 < n && constant_slots[op2]);
            int op1_val = op1_const ? mem[op1] : 0;
            int op2_val = op2_const ? mem[op2] : 0;

            ArithmeticOperatorType op = pc->assign_inst.op;
            bool did_simplify = true;

            if (op == OPERATOR_PLUS)
            {
                if (op2_const && op2_val == 0)  // x + 0 = x
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else if (op1_const && op1_val == 0)  // 0 + x = x
                {
                    pc->assign_inst.operand1_index = op2;
                }
                else 
                {
                    did_simplify = false;
                }
            }
            else if (op == OPERATOR_MINUS)
            {
                if (op2_const && op2_val == 0)  // x - 0 = x
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else if (op1 == op2)  // x - x = 0
                {
                    pc->assign_inst.operand1_index = zero_slot;
                }
                else
                {
                    did_simplify = false;
                }
            }
            else if (op == OPERATOR_MULT)
            {
                if (op2_const && op2_val == 1)  // x * 1 = x
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else if (op1_const && op1_val == 1)  // 1 * x = x
                {
                    pc->assign_inst.operand1_index = op2;
                }
                else if (op2_const && op2_val == 0)  // x * 0 = 0
                {
                    pc->assign_inst.operand1_index = zero_slot;
                }
                else if (op1_const && op1_val == 0)  // 0 * x = 0
                {
                    pc->assign_inst.operand1_index = zero_slot;
                }
                else
                {
                    did_simplify = false;
                }
            }
            else if (op == OPERATOR_DIV)
            {
                if (op2_const && op2_val == 1)
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else 
                {
                    did_simplify = false;
                }
            }
            else
            {
                did_simplify = false;
            }

            if (did_simplify)
            {
                pc->assign_inst.op = OPERATOR_NONE;
                simplified++;
            }
        }
        pc = pc->next;
    }
    return simplified;
}
