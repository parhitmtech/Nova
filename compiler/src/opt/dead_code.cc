/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Optimizer: dead code elimination. Split out of compiler.cc.
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

int dead_code_eliminate(struct InstructionNode* program)
{
    int n = next_available;

    // Pass 1 - mark all slots that are read anywhere
    vector<bool> is_read(n, false);
    
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        switch (pc->type)
        {
            case ASSIGN:
            {
                if (pc->assign_inst.op == OPERATOR_NONE)
                {
                    int src = pc->assign_inst.operand1_index;
                    if (src < n)
                    {
                        is_read[src] = true;
                    }
                }
                else 
                {
                    int op1 = pc->assign_inst.operand1_index;
                    int op2 = pc->assign_inst.operand2_index;
                    if (op1 < n) is_read[op1] = true;
                    if (op2 < n) is_read[op2] = true;
                }
                break;
            }
            case OUT:
            {
                if (pc->output_inst.var_index < n)
                {
                    is_read[pc->output_inst.var_index] = true;
                }
                break;
            }
            case CJMP:
            {
                if (pc->cjmp_inst.operand1_index < n) is_read[pc->cjmp_inst.operand1_index] = true;
                if (pc->cjmp_inst.operand2_index < n) is_read[pc->cjmp_inst.operand2_index] = true;
                break;
            }
            case SCMP:
            {
                if (pc->scmp_inst.operand1_index < n) is_read[pc->scmp_inst.operand1_index] = true;
                if (pc->scmp_inst.operand1_index < n) is_read[pc->scmp_inst.operand1_index] = true;
                break;
            }
            case ARRAY_READ:
            {
                if (pc->array_inst.index_slot < n) is_read[pc->array_inst.index_slot] = true;
                if (pc->array_inst.base_index < n) is_read[pc->array_inst.base_index] = true;
                if (pc->array_inst.size_slot >= 0 && pc->array_inst.size_slot < n)
                {
                    is_read[pc->array_inst.size_slot] = true;
                }
                break;
            }
            case ARRAY_WRITE:
            {
                if (pc->array_inst.index_slot < n) is_read[pc->array_inst.index_slot] = true;
                if (pc->array_inst.target_index < n) is_read[pc->array_inst.target_index] = true;
                if (pc->array_inst.base_index < n) is_read[pc->array_inst.base_index] = true;
                if (pc->array_inst.size_slot >= 0 && pc->array_inst.size_slot < n)
                {
                    is_read[pc->array_inst.size_slot] = true;
                }
                break;
            }
            case CALL:
            {
                for (int i = 0;i < pc->call_inst.num_params;i++)
                {
                    if (pc->call_inst.arg_val_slots[i] < n)
                    {
                        is_read[pc->call_inst.arg_val_slots[i]] = true;
                    }
                }
                if (pc->call_inst.ret_val_index < n)
                {
                    is_read[pc->call_inst.ret_val_index] = true;
                }
                break;  
            }
            case RET:
            {
                if (pc->ret_inst.ret_val_index < n)
                {
                    is_read[pc->ret_inst.ret_val_index] = true;
                }
                break;
            }
            case ALLOC:
            {
                if (pc->alloc_inst.size_slot < n) is_read[pc->alloc_inst.size_slot] = true;
                if (pc->alloc_inst.base_slot < n) is_read[pc->alloc_inst.base_slot] = true;
                break;
            }
            case STRCAT:
            {
                if (pc->strcat_inst.left_slot < n) is_read[pc->strcat_inst.left_slot] = true;
                if (pc->strcat_inst.right_slot < n) is_read[pc->strcat_inst.right_slot] = true;
                break;
            }
            default: break;
        }
        pc = pc->next;
    }

    // Pass 2 - eliminate OPERATOR_NONE assignments to unread slots
    int eliminated = 0;
    pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op == OPERATOR_NONE)
        {
            int dst = pc->assign_inst.left_hand_side_index;
            if (dst < n && !is_read[dst])
            {
                pc->type = NOOP;
                eliminated++;
            }
        }
        pc = pc->next;
    }
    return eliminated;
}
 
