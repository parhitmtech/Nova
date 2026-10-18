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
#include "regalloc.h"  // get_defs

using namespace std;

// Constant folding
//
// constant_slots[i] means "mem[i] already holds, at compile time, the value slot i has whenever it is
// read at run time". That is true for slots nothing ever writes (literals the parser stored), and for
// slots written exactly once by a copy or a foldable operation of such slots; mark_constants() and
// fold_pass() establish the latter by computing those values into mem[] now. A slot that is merely
// assigned once (e.g. `int x = 10 ;`) still holds 0 at compile time until its ASSIGN is evaluated, so
// "written once" alone is not enough.

static vector<bool> constant_slots;
static vector<int> slot_writes;  // static count of instructions (in any body) that write each mem slot

// Every mem[] slot n writes. get_defs() covers ASSIGN/IN/ARRAY_READ/CALL/ALLOC/STRCAT/TENSOR_CALL.
static void count_writes(struct InstructionNode* n)
{
    std::unordered_set<int> defs;
    get_defs(n, defs);
    switch (n->type)
    {
        case CAST:
            if (n->cast_inst.dst_type != TYPE_FLOAT && n->cast_inst.dst_type != TYPE_DOUBLE)
                defs.insert(n->cast_inst.dst_index);
            break;
        case HF_INFER:
            if (n->hf_inst.result_slot >= 0) defs.insert(n->hf_inst.result_slot);
            break;
        case SOL_CALL:
            if (n->sol_inst.job_slot >= 0) defs.insert(n->sol_inst.job_slot);
            if (n->sol_inst.result_slot >= 0) defs.insert(n->sol_inst.result_slot);
            break;
        default:
            break;
    }
    for (int d : defs)
        if (d >= 0 && d < (int)slot_writes.size()) slot_writes[d]++;
}

static bool written_once(int slot)
{
    return slot >= 0 && slot < (int)slot_writes.size() && slot_writes[slot] == 1;
}

static bool known(int slot)
{
    return slot >= 0 && slot < (int)constant_slots.size() && constant_slots[slot];
}

// Plain copies `dst <- src` of a known value into a slot written only by that copy make dst known.
static int propagate_known_copies(struct InstructionNode* program)
{
    int changed = 0;
    for (struct InstructionNode* pc = program; pc != nullptr; pc = pc->next)
    {
        if (pc->type != ASSIGN || pc->assign_inst.op != OPERATOR_NONE) continue;
        int dst = pc->assign_inst.left_hand_side_index;
        int src = pc->assign_inst.operand1_index;
        if (known(dst) || !written_once(dst) || !known(src)) continue;
        mem[dst] = mem[src];
        constant_slots[dst] = true;
        changed++;
    }
    return changed;
}

void mark_constants(struct InstructionNode* program)
{
    int n = next_available;
    slot_writes.assign(n, 0);
    // Count writes everywhere a slot can be written: main, every function and every class method.
    std::set<struct InstructionNode*> bodies = {program};
    for (auto& kv : functionTable) bodies.insert(kv.second);
    for (auto& cls : classMethodTable)
        for (auto& m : cls.second) bodies.insert(m.second);
    for (auto body : bodies)
        for (struct InstructionNode* pc = body; pc != nullptr; pc = pc->next) count_writes(pc);

    constant_slots.assign(n, false);
    for (int i = 0; i < n; i++)
        if (slot_writes[i] == 0) constant_slots[i] = true;  // literal / never written
    while (propagate_known_copies(program) > 0) {}
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

            if (known(op1_idx) && known(op2_idx) && written_once(dst_idx))
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
    } while (folds > 0 || propagate_known_copies(program) > 0);
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
