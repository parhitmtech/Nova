/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Optimizer: copy propagation. Split out of compiler.cc.
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

int copy_propagate(struct InstructionNode* program)
{
    int n = next_available;

    // count writes per slot since slots written more than once are undafe to propogate
    vector<int> write_count(n, 0);
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN)
        {
            write_count[pc->assign_inst.left_hand_side_index]++;
        }
        if (pc->type == IN)
        {
            write_count[pc->input_inst.var_index]++;
        }
        if (pc->type == ARRAY_READ)
        {
            write_count[pc->array_inst.target_index]++;
        }
        pc = pc->next;
    }

    // copy_map[slot] = slot it is a direct copy of
    vector<int> copy_map(n, -1);  // -1 = not a copy

    auto resolve = [&](int slot) -> int {
        if (slot < n && copy_map[slot] != -1) return copy_map[slot];
        return slot;
    };

    int propogated = 0;
    pc = program;
    while (pc != nullptr)
    {
        switch (pc->type)
        {
            case ASSIGN: 
            {
                int dst = pc->assign_inst.left_hand_side_index;
                if (pc->assign_inst.op == OPERATOR_NONE)
                {
                    // resolve src through map first
                    int src = resolve(pc->assign_inst.operand1_index);
                    pc->assign_inst.operand1_index = src;

                    // only record as a copy if slot is written exactly once
                    if (dst != src && dst < n && write_count[dst] == 1 && (src >= n || write_count[src] == 1))
                    {
                        copy_map[dst] = src;
                    }
                    else
                    {
                        copy_map[dst] = -1; // unsafe, so invalidate
                    }
                }
                else
                {
                    // arithmetic node - resolve both operands
                    int op1 = resolve(pc->assign_inst.operand1_index);
                    int op2 = resolve(pc->assign_inst.operand2_index);
                    if (op1 != pc->assign_inst.operand1_index) 
                    {
                        pc->assign_inst.operand1_index = op1;
                        propogated++;
                    }
                    if (op2 != pc->assign_inst.operand2_index)
                    {
                        pc->assign_inst.operand2_index = op2;
                    }
                    // dst is now computed - no longer a simple copy
                    if (dst < n) copy_map[dst] = -1;
                }
                break;
            }
            case IN:
                // runtime input - invalidate
                if (pc->input_inst.var_index < n)
                {
                    copy_map[pc->input_inst.var_index] = -1;
                }
                break;
            
            case OUT:
            {
                int resolved = resolve(pc->output_inst.var_index);
                if (resolved != pc->output_inst.var_index)
                {
                    pc->output_inst.var_index = resolved;   
                    propogated++;
                }
                break;
            }
            case CJMP:
            {
                int op1 = resolve(pc->cjmp_inst.operand1_index);
                int op2 = resolve(pc->cjmp_inst.operand2_index);
                if (op1 != pc->cjmp_inst.operand1_index)
                {
                    pc->cjmp_inst.operand1_index = op1;
                    propogated++;
                }
                if (op2 != pc->cjmp_inst.operand2_index)
                {
                    pc->cjmp_inst.operand2_index = op2;
                    propogated++;
                }
                break;
            }
            case SCMP:
            {
                int op1 = resolve(pc->scmp_inst.operand1_index);
                int op2 = resolve(pc->scmp_inst.operand2_index);    
                if (op1 != pc->scmp_inst.operand1_index)
                {
                    pc->scmp_inst.operand1_index = op1;
                    propogated++;
                }
                if (op2 != pc->scmp_inst.operand2_index)
                {
                    pc->scmp_inst.operand2_index = op2;
                    propogated++;
                }
                break;
            }
            case ARRAY_READ:
            {
                int resolved = resolve(pc->array_inst.index_slot);
                if (resolved != pc->array_inst.index_slot)
                {
                    pc->array_inst.index_slot = resolved;
                    propogated++;
                }
                if (pc->array_inst.target_index < n)
                {
                    copy_map[pc->array_inst.target_index] = -1;
                }
                break;
            }
            case ARRAY_WRITE:
            {
                int idx = resolve(pc->array_inst.index_slot);
                int val = resolve(pc->array_inst.target_index);
                if (idx != pc->array_inst.index_slot)
                {
                    pc->array_inst.index_slot = idx;
                    propogated++;
                }
                if (val != pc->array_inst.target_index)
                {
                    pc->array_inst.target_index = val;
                    propogated++;
                }
                break;
            }
            case CALL:
            {
                for (int i = 0;i < pc->call_inst.num_params;i++)
                {
                    int resolved = resolve(pc->call_inst.arg_val_slots[i]);
                    if (resolved != pc->call_inst.arg_val_slots[i])
                    {
                        pc->call_inst.arg_val_slots[i] = resolved;
                        propogated++;
                    }
                    // param slots get new values - invalidate
                    int p = pc->call_inst.param_slots[i];
                    if (p < n) copy_map[p] = -1;
                }
                // The callee can write any slot (a method writes its self slots, which are then copied
                // back into the object), and those writes are not counted in write_count. So no copy
                // recorded before the call still holds after it.
                std::fill(copy_map.begin(), copy_map.end(), -1);
                break;
            }
            case RET:
            {
                int resolved = resolve(pc->ret_inst.ret_val_index);
                if (resolved != pc->ret_inst.ret_val_index)
                {
                    pc->ret_inst.ret_val_index = resolved;
                    propogated++;
                }
                break;
            }
            default: break;
        }
        pc = pc->next;
    }
    return propogated;
}
