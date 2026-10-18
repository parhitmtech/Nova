/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Optimizer: function inlining. Split out of compiler.cc.
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

// The inliner only remaps mem[] (int) slots and copies arguments and the return value with int
// ASSIGNs, so it may only inline calls where everything lives in mem[]. Anything touching fmem[] /
// dmem[] (float/double), tensors, nested calls or multiple return slots is left as a real CALL.
static bool is_int_like(VarType t)
{
    return t != TYPE_FLOAT && t != TYPE_DOUBLE && t != TYPE_TENSOR;
}

static bool inlinable_call(struct InstructionNode* call, int bodyNodes)
{
    if (!is_int_like(call->call_inst.ret_type) || call->call_inst.num_ret_slots > 1) return false;
    for (int i = 0; i < call->call_inst.num_params; i++)
    {
        if (call->call_inst.param_types && !is_int_like(call->call_inst.param_types[i])) return false;
        if (call->call_inst.arg_types && !is_int_like(call->call_inst.arg_types[i])) return false;
    }
    struct InstructionNode* n = call->call_inst.function_head;
    for (int i = 0; i < bodyNodes && n != nullptr; i++, n = n->next)
    {
        switch (n->type)
        {
            case ASSIGN: case IN: case ARRAY_READ: case ARRAY_WRITE: case NOOP:
                break;
            case OUT:
                if (!is_int_like(n->output_inst.value_type)) return false;
                break;
            case RET:
                if (!is_int_like(n->ret_inst.ret_type)) return false;
                break;
            default:  // ASSIGN_F/ASSIGN_D/CAST/CALL/TENSOR_CALL/STRCAT/ALLOC/...: not remapped below
                return false;
        }
    }
    return true;
}

int inline_functions(struct InstructionNode*& program)
{
    const int MAX_CALL_SITES = 3;
    const int MAX_BODY_NODES = 20;

    int inlined = 0;

    // Step 1: gather info about every function
    // count call sites and body size for each function head pointer

    map<struct InstructionNode*, int> callSiteCount;
    map<struct InstructionNode*, int> bodySize;

    // count call sites
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == CALL && pc->call_inst.function_head != nullptr)
        {
            callSiteCount[pc->call_inst.function_head]++;
        }
        pc = pc->next;
    }

    // measure body size (up to and including RET)
    for (auto& kv : callSiteCount)
    {
        struct InstructionNode* head = kv.first;
        int count = 0;
        struct InstructionNode* n = head;
        while (n != nullptr)
        {
            count++;
            if (n->type == RET) break;
            n = n->next;
        }
        bodySize[head] = count;
    }

    // detect recursive functions - scan body for a CALL back to itself
    set<struct InstructionNode*> isRecursive;
    for (auto& kv : callSiteCount)
    {
        struct InstructionNode* head = kv.first;
        struct InstructionNode* n = head;
        int sz = bodySize.count(head) ? bodySize[head] : 0;
        for (int i = 0;i < sz && n != nullptr;i++, n = n->next)
        {
            if (n->type == CALL && n->call_inst.function_head == head)
            {
                isRecursive.insert(head);
                break;
            }
        } 
    }

    // Step 2: Inline eligible call sites
    // we need a pointer to the node before each call to rewire the list
    // We can iterate with a prev pointer

    struct InstructionNode* prev = nullptr;
    pc = program;

    while (pc != nullptr)
    {
        if (pc->type != CALL || pc->call_inst.function_head == nullptr)
        {
            prev = pc;
            pc = pc->next;  
            continue;
        }

        struct InstructionNode* funcHead = pc->call_inst.function_head;
        
        bool hasControlFlow = false;
        {
            struct InstructionNode* bodyCheck = funcHead;
            int sz = bodySize.count(funcHead) ? bodySize[funcHead] : 0;
            for (int i = 0;i < sz && bodyCheck != nullptr; i++, bodyCheck = bodyCheck->next)
            {
                if (bodyCheck->type == CJMP || bodyCheck->type == JMP || bodyCheck->type == SCMP)
                {
                    hasControlFlow = true;
                    break;
                }
            }
        }

        // check eligibility
        bool eligible = !isRecursive.count(funcHead) && !hasControlFlow && callSiteCount[funcHead] <= MAX_CALL_SITES && bodySize[funcHead] <= MAX_BODY_NODES
                        && inlinable_call(pc, bodySize[funcHead]);

        if (!eligible)
        {
            prev = pc;
            pc = pc->next;
            continue;
        }

        // Step 3: build slot remapping
        int base = pc->call_inst.func_slot_base;
        int count = pc->call_inst.func_slot_count;
        int retValIdx = pc->call_inst.ret_val_index;

        map<int, int>slotMap;
        for (int i = 0;i < count;i++)
        {
            int newSlot = alloc_slot();
            mem[newSlot] = mem[base + i];
            slotMap[base + i] = newSlot;
        }

        // remap helper
        auto remap = [&](int slot) -> int {
            auto it = slotMap.find(slot);
            return (it != slotMap.end()) ? it->second : slot;
        };

        // Step 4: emit arg copies into remapped param slots
        // Build a small chain of ASSIGN nodes: remapped_param <- arg_val
        struct InstructionNode* chainHead = nullptr;
        struct InstructionNode* chainTracker = nullptr;

        auto chain_append = [&](struct InstructionNode* node) {
            node->next = nullptr;
            if (chainHead == nullptr) chainHead = chainTracker = node;
            else {chainTracker->next = node; chainTracker = node;}
        };

        for (int i = 0;i < pc->call_inst.num_params;i++)
        {
            int remappedParam = remap(pc->call_inst.param_slots[i]);
            int argSlot = pc->call_inst.arg_val_slots[i];

            struct InstructionNode* argCopy = new InstructionNode();
            argCopy->type = ASSIGN;
            argCopy->assign_inst.left_hand_side_index = remappedParam;
            argCopy->assign_inst.operand1_index = argSlot;
            argCopy->assign_inst.op = OPERATOR_NONE;
            chain_append(argCopy);
        }

        // Step 5: deep copy function with slot remapping
        struct InstructionNode* callNext = pc->next;
        int destSlot = pc->call_inst.ret_val_index;  // where caller expects result

        struct InstructionNode* bodyNode = funcHead;
        while (bodyNode != nullptr)
        {
            struct InstructionNode* clone = new InstructionNode();
            *clone = *bodyNode;  // shallow copy all fields
            clone->next = nullptr;

            if (clone->type == ASSIGN)
            {
                clone->assign_inst.left_hand_side_index = remap(clone->assign_inst.left_hand_side_index);
                clone->assign_inst.operand1_index = remap(clone->assign_inst.operand1_index);
                if (clone->assign_inst.op != OPERATOR_NONE)
                {
                    clone->assign_inst.operand2_index = remap(clone->assign_inst.operand2_index);
                }
            }
            else if (clone->type == RET)
            {
                // convert RET -> ASSIGN: destslot <- remapped ret val
                clone->type = ASSIGN;
                clone->assign_inst.left_hand_side_index = destSlot;
                clone->assign_inst.operand1_index = remap(bodyNode->ret_inst.ret_val_index);
                clone->assign_inst.op = OPERATOR_NONE;
                chain_append(clone);
                break;  // stop at first REt
            }
            else if (clone->type == CJMP)
            {
                clone->cjmp_inst.operand1_index = remap(clone->cjmp_inst.operand1_index);
                clone->cjmp_inst.operand2_index = remap(clone->cjmp_inst.operand2_index);
            }
            else if (clone->type == SCMP)
            {
                clone->scmp_inst.operand1_index = remap(clone->scmp_inst.operand1_index);
                clone->scmp_inst.operand2_index = remap(clone->scmp_inst.operand2_index);
            }
            else if (clone->type == IN)
            {
                clone->input_inst.var_index = remap(clone->input_inst.var_index);   
            }
            else if (clone->type == OUT)
            {
                clone->output_inst.var_index = remap(clone->output_inst.var_index);
            }
            else if (clone->type == ARRAY_READ)
            {
                clone->array_inst.base_index = remap(clone->array_inst.base_index);
                clone->array_inst.index_slot = remap(clone->array_inst.index_slot);
                clone->array_inst.target_index = remap(clone->array_inst.target_index);
            }
            else if (clone->type == ARRAY_WRITE)
            {
                clone->array_inst.base_index = remap(clone->array_inst.base_index);
                clone->array_inst.index_slot = remap(clone->array_inst.index_slot);
                clone->array_inst.target_index = remap(clone->array_inst.target_index);
            }

            chain_append(clone);

            if (bodyNode->type == RET)
            {
                break;
            }
            bodyNode = bodyNode->next;
        }

        // Step 6: wire the cloned chain into the IR
        // chainTracker->callNext (skip the original CALL node)
        if (chainTracker != nullptr)
        {
            chainTracker->next = callNext;
        }

        if (prev == nullptr)
        {
            program = chainHead;  // inlining at very start of the program
        }
        else
        {
            prev->next = chainHead;
        }

        // original CALL node is now bypassed - move forward
        pc = callNext;
        // prev stays pointing to chainTracker (last inlined node)
        prev = chainTracker;

        inlined++;
    }
    return inlined;
}
