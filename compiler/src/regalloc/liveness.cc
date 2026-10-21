/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Liveness analysis (defs/uses/successors per IR node). Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "lexer.h"
#include "compiler.h"
#include "regalloc.h"

using namespace std;

// ── Graph Coloring Register Allocation ─────────────────────────────────────────────────────────

// ── Liveness Analysis ─────────────────────────────────────────────────────────

// mem[], fmem[], and dmem[] are the three independent, seperately-numbered slot spaces. A TENSOR_CALL's 
// result_slot is NOT always a mem[] index: ops in TENSOR_DOUBLE_OPS (parser.cc) allocate their result from
// dmem[] via alloc_double_slot() instead of alloc_slot, e.g. tensor_get()'s result is a scalar double,
// not a tensor handle. Treating that dmem[] index as a mem[] "def" here is wrong on its own terms 
// (register allocation only colors mem[] slots) and actively dangerous for the tensor-free pass
// (get_tensor_release_sites below, which shares this same list): a dmem[] index can numerically 
// collide with an unrelated, genuinely tensor-tained mem[] slot, and get_tensor_release_sites would then 
// place a tensor-tainted mem[] slot, and get_tensor_release_sites would then place a TENSOR_RELEASE
// at the dmem value's last use instead of the real tensor slot's
bool tensor_call_result_is_mem_slot(TensorOp op)
{
    switch (op) 
    {
        case TEN_SUM: case TEN_MEAN: case TEN_MAX: case TEN_GET: case CON_GET_D: case CON_POP_D: case CON_MIN_D: case CON_MAX_D: case CON_SUM_D:
        {
            return false;  // dmem[] result
        }
        default:
        {
            return true;
        }
    }
} 

// Get all slots DEFINED (written) by an instruction
void get_defs(struct InstructionNode* n, std::unordered_set<int>& defs)
{
    switch (n->type)
    {
        case ASSIGN:
        {
            defs.insert(n->assign_inst.left_hand_side_index);
            break;
        }
        case IN:
        {
            defs.insert(n->input_inst.var_index);
            break;
        }
        case ARRAY_READ:
        {
            defs.insert(n->array_inst.target_index);
            break;
        }
        case CALL:
        {
            // ret_val_index/all_ret_slots/param_slots are mem[]-space only when the corresponding
            // return/param is int-typed (or untyped, which the interpreter's own param-binding loop
            // also defaults to mem[] -- see its pt == TYPE_UNKNOWN fallback). A float or double one lives
            // in fmem[]/dmem[] instead, a seperately-numbered space, so including it here risks the same
            // kind of cross-space slot-number collision described in tensor_call_result_is_mem_slot() above.
            defs.insert(n->call_inst.ret_val_index);
            if (n->call_inst.all_ret_slots)
            {
                for (int i = 0;i < n->call_inst.num_ret_slots;i++)
                {
                    defs.insert(n->call_inst.all_ret_slots[i]);    
                }
            }
            // param slots are written by CALL
            for (int i = 0;i < n->call_inst.num_params;i++)
            {
                VarType pt = n->call_inst.param_types ? n->call_inst.param_types[i] : TYPE_UNKNOWN;
                if (pt == TYPE_FLOAT || pt == TYPE_DOUBLE) continue;
                defs.insert(n->call_inst.param_slots[i]);
            }
            break;
        }
        case ALLOC:
        {
            defs.insert(n->alloc_inst.base_slot);
            break;
        }
        case STRCAT:
        {
            defs.insert(n->strcat_inst.dest_slot);
            break;
        }
        case TENSOR_CALL:
        {
            if (n->tensor_call_inst.result_slot >= 0 &&
                tensor_call_result_is_mem_slot(n->tensor_call_inst.op))
            {
                defs.insert(n->tensor_call_inst.result_slot);
            }
            break;
        }
        default: break;
    }
}

// Get all slots USED (read) by an instruction
void get_uses(struct InstructionNode* n, std::unordered_set<int>& uses)
{
    switch (n->type)
    {
        case ASSIGN:
        {
            uses.insert(n->assign_inst.operand1_index);
            if (n->assign_inst.op != OPERATOR_NONE)
            {
                uses.insert(n->assign_inst.operand2_index);
            }
            break;
        }
        case OUT:
        {
            // a float/double value lives in fmem[]/dmem[], not mem[]
            if ((n->output_inst.value_type != TYPE_FLOAT) && (n->output_inst.value_type != TYPE_DOUBLE))
            {
                uses.insert(n->output_inst.var_index);
            }
            break;
        }
        case CJMP:
        {
            // a float/double comparison reads fmem[]/dmem[], not mem[]
            if (n->cjmp_inst.operand_type != TYPE_FLOAT && n->cjmp_inst.operand_type != TYPE_DOUBLE)
            {
                uses.insert(n->cjmp_inst.operand1_index);
                uses.insert(n->cjmp_inst.operand2_index);
            }
            break;
        }
        case SCMP:
        {
            uses.insert(n->scmp_inst.operand1_index);
            uses.insert(n->scmp_inst.operand2_index);
            break;
        }
        case ARRAY_READ:
        {
            uses.insert(n->array_inst.index_slot);
            if (n->array_inst.dynamic_base)
            {
                uses.insert(n->array_inst.base_index);
            }
            break;
        }
        case ARRAY_WRITE:
        {
            uses.insert(n->array_inst.index_slot);
            uses.insert(n->array_inst.target_index);
            if (n->array_inst.dynamic_base)
            {
                uses.insert(n->array_inst.base_index);
            }
            break;
        }
        case CALL:
        {
            // Same mem[]/fmem[]/dmem[] space caveat as CALL's defs case above.
            for (int i = 0;i < n->call_inst.num_params;i++)
            {
                VarType at = n->call_inst.arg_types ? n->call_inst.arg_types[i] : TYPE_UNKNOWN;
                if (at == TYPE_FLOAT || at == TYPE_DOUBLE) continue;
                uses.insert(n->call_inst.arg_val_slots[i]);
            }
            break;
        }
        case RET:
        {
            // a float/double return slot lives in fmem[]/dmem[], not mem[]
            if (n->ret_inst.ret_type != TYPE_FLOAT && n->ret_inst.ret_type != TYPE_DOUBLE)
            {
                uses.insert(n->ret_inst.ret_val_index);
            }
            break;
        }
        case ALLOC:
        {
            uses.insert(n->alloc_inst.size_slot);
            break;
        }
        case STRCAT:
        {
            uses.insert(n->strcat_inst.left_slot);
            uses.insert(n->strcat_inst.right_slot);
            break;
        }
        case TENSOR_CALL:
        {
            // See tensor_call_result_is_mem_slot()'s comment above get_defs;
            //arg_slots mixes mem[]/fmem[]/dmem indices per arg_types[i]
            // (e.g. TEN_SET's value argument), so a non-mem[]-space arg must be skipped here too,
            // or it can be numerically mistaken for an unrelated mem[] slot by every consumer of
            // this liveness result.
            for (int i = 0;i < n->tensor_call_inst.num_args;i++)
            {
                VarType at = n->tensor_call_inst.arg_types[i];
                if (at == TYPE_FLOAT || at == TYPE_DOUBLE) continue;
                uses.insert(n->tensor_call_inst.arg_slots[i]);
            }
            break;
        }
        case TENSOR_RELEASE:
        {
            uses.insert(n->tensor_release_inst.slot);
            break;
        }
        default: break;
    }
}

// Get successor nodes of n (for CFG edges)
void get_successors(struct InstructionNode* n, std::vector<struct InstructionNode*>& succs)
{
    if (n == nullptr) return;
    switch (n->type)
    {
        case JMP:
        {
            if (n->jmp_inst.target) succs.push_back(n->jmp_inst.target);
            break;
        }
        case CJMP:
        {
            if (n->next) succs.push_back(n->next);  // condition true
            if (n->cjmp_inst.target) succs.push_back(n->cjmp_inst.target);  // condition false
            break;
        }
        case RET:
        {
            break;  // no successors
        }
        default:
        {
            if (n->next) succs.push_back(n->next);
            break;
        }
    }
}

LivenessResult compute_liveness(struct InstructionNode* program)
{
    // Collect all nodes into a list
    std::vector<struct InstructionNode*> nodes;
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        nodes.push_back(pc);
        pc = pc->next;
    }

    LivenessResult result;

    // Initialize live_in and live_out as empty sets
    for (auto n : nodes)
    {
        result.live_in[n] = {};
        result.live_out[n] = {};
    }

    // Iterative backward dataflow until convergence
    bool changed = true;
    while (changed)
    {
        changed = false;
        // Process nodes in reverse order (backward analysis)
        for (int i = (int)nodes.size() - 1;i >= 0;i--)
        {
            struct InstructionNode* n = nodes[i];

            // live_out[n] = union of live_in[successors]
            std::unordered_set<int> new_out;
            std::vector<struct InstructionNode*> succs;
            get_successors(n, succs);
            for (auto s : succs)
            {
                for (int slot : result.live_in[s])
                {
                    new_out.insert(slot);
                }
            }

            // live_in[n] = use[n] union (live_out[n] - def[n])
            std::unordered_set<int> defs, uses;
            get_defs(n, defs);
            get_uses(n, uses);

            std::unordered_set<int>new_in = uses;
            for (int slot : new_out)
            {
                if (!defs.count(slot))
                {
                    new_in.insert(slot);
                }
            }
            // Check if anything changed
            if (new_in != result.live_in[n] || new_out != result.live_out[n])
            {
                result.live_in[n] = new_in;
                result.live_out[n] = new_out;
                changed = true;
            }
        }
    }
    return result;
}

void dump_liveness(struct InstructionNode*, const LivenessResult&) {}
