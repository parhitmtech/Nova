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
#include "regalloc.h"  // get_uses

using namespace std;

// Every mem[] slot n may read. get_uses() covers the core instructions (including tensor-call
// arguments); this adds the rest. Over-approximating is safe: it only keeps more assignments alive.
static void collect_reads(struct InstructionNode* n, vector<bool>& is_read)
{
    std::unordered_set<int> uses;
    get_uses(n, uses);
    switch (n->type)
    {
        case CALL:  // multi-return values are read back from every return slot
            uses.insert(n->call_inst.ret_val_index);
            for (int i = 0; n->call_inst.all_ret_slots && i < n->call_inst.num_ret_slots; i++)
                uses.insert(n->call_inst.all_ret_slots[i]);
            break;
        case CAST:
            uses.insert(n->cast_inst.src_index);
            break;
        case HF_INFER:
            uses.insert(n->hf_inst.model_slot);
            uses.insert(n->hf_inst.input_slot);
            break;
        case SOL_CALL:
            for (int s : {n->sol_inst.job_slot, n->sol_inst.model_slot, n->sol_inst.task_slot,
                          n->sol_inst.dataset_slot, n->sol_inst.input_slot, n->sol_inst.path_slot})
                uses.insert(s);
            break;
        default:
            break;
    }
    for (int u : uses)
        if (u >= 0 && u < (int)is_read.size()) is_read[u] = true;
}

// Removes plain copies in the main program whose destination slot is never read anywhere
// (main, functions or class methods).
int dead_code_eliminate(struct InstructionNode* program)
{
    int n = next_available;

    // Pass 1 - mark all slots that are read anywhere
    vector<bool> is_read(n, false);
    std::set<struct InstructionNode*> bodies = {program};
    for (auto& kv : functionTable) bodies.insert(kv.second);
    for (auto& cls : classMethodTable)
        for (auto& m : cls.second) bodies.insert(m.second);
    for (auto body : bodies)
        for (struct InstructionNode* node = body; node != nullptr; node = node->next)
            collect_reads(node, is_read);

    // Pass 2 - eliminate OPERATOR_NONE assignments to unread slots. Copies of tensor handles and
    // stores into object fields / self slots are not pure: the interpreter retains/releases tensor
    // references on them, so removing one changes when tensors are freed. Keep those.
    mark_tensor_slots(program);
    auto has_side_effects = [](struct InstructionNode* a) {
        int dst = a->assign_inst.left_hand_side_index, src = a->assign_inst.operand1_index;
        return g_tensorSlots.count(src) || g_tensorSlots.count(dst) || g_fieldSlots.count(dst) ||
               g_selfSlots.count(dst);
    };
    int eliminated = 0;
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op == OPERATOR_NONE && !has_side_effects(pc))
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
 
