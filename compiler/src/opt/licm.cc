/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Optimizer: loop-invariant code motion. Split out of compiler.cc.
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

// Considers nested loop structures as well
int loop_invariant_code_motion(struct InstructionNode* program)
{
    int hoisted = 0;

    // Step 1: number all nodes by position
    map<struct InstructionNode*, int> nodeIndex;
    vector<struct InstructionNode*> nodeList;
    struct InstructionNode* pc = program;
    int idx = 0;
    while (pc != nullptr)
    {
        nodeIndex[pc] = idx++;
        nodeList.push_back(pc);
        pc = pc->next;
    }
    int total = idx;

    // Step 2: find all back-edges (loops)
    // Back edge: a jmp node whose whose target has a lower index than itself
    // Process loops from inner most to outermost by sorting the header index descendant
    struct LoopInfo {
        int header_idx;  // index of loop header
        int jmp_idx;  // index of the back-edge JMP node
    };
    vector<LoopInfo> loops;

    for (int i = 0;i < total;i++)
    {
        struct InstructionNode* node = nodeList[i];
        if (node->type == JMP)
        {
            struct InstructionNode* target = node->jmp_inst.target;
            if (target != nullptr && nodeIndex.count(target))
            {
                int target_idx = nodeIndex[target];
                if (target_idx < i)  // back-edge 
                {
                    loops.push_back({target_idx, i});
                }
            }
        }
    }

    // sort innermost first (highest header index = deepest nesting)
    sort(loops.begin(), loops.end(), [](const LoopInfo& a, const LoopInfo& b) {
        return a.header_idx > b.header_idx;
    });

    // Step 3: process each loop
    for (auto& loop : loops)
    {
        struct InstructionNode* header = nodeList[loop.header_idx];
        struct InstructionNode* jmpNode = nodeList[loop.jmp_idx];

        int body_start = loop.header_idx;
        int body_end = loop.jmp_idx;

        // collect set of slots written anywhere in the loop body
        set<int> written_in_loop;
        for (int i = body_start;i <= body_end;i++)
        {
            struct InstructionNode* n = nodeList[i];
            if (n->type == ASSIGN)
            {
                written_in_loop.insert(n->assign_inst.left_hand_side_index);
            }
            if (n->type == IN)
            {
                written_in_loop.insert(n->input_inst.var_index);
            }
            if (n->type == ARRAY_READ)
            {
                written_in_loop.insert(n->array_inst.target_index);
            }
        }   

        // count writes per slot within the loop body
        map<int, int> loop_write_count;
        for (int i = body_start;i <= body_end;i++)
        {
            struct InstructionNode* n = nodeList[i];
            if (n->type == ASSIGN)
            {
                loop_write_count[n->assign_inst.left_hand_side_index]++;
            }
        }

        // Step 4 - find and hoist invariant nodes
        // iterate body_start+1 to body_end-1 (skip header and jump itself)
        for (int i = body_start+1;i < body_end;i++)
        {
            struct InstructionNode* n = nodeList[i];
            if (n->type != ASSIGN || n->assign_inst.op == OPERATOR_NONE)
            {
                continue;
            }

            int op1 = n->assign_inst.operand1_index;
            int op2 = n->assign_inst.operand2_index;
            int dst = n->assign_inst.left_hand_side_index;

            // invariant condition: neither operand is written in loop and dst is written exactly once in loop 
            bool op1_safe = (written_in_loop.find(op1) == written_in_loop.end());
            bool op2_safe = (written_in_loop.find(op2) == written_in_loop.end());
            bool dst_once = (loop_write_count[dst] == 1);

            if (!op1_safe || !op2_safe || !dst_once) continue;

            // hoist: remove from loop body, insert before header

            // find the node just before n in the linked list
            struct InstructionNode* prev = nullptr;
            struct InstructionNode* curr = program;
            while (curr != nullptr && curr->next != n)
            {
                prev = curr;
                curr = curr->next;
            }
            if (prev == nullptr) continue;  // cannot hoist first node

            // unlink n from its current position
            prev->next = curr->next;

            // find the node just before header
            struct InstructionNode* beforeHeader = nullptr;
            curr = program;

            while (curr != nullptr && curr->next != nullptr)
            {
                beforeHeader = curr;
                curr = curr->next;
            }

            if (beforeHeader == nullptr)
            {
                // header is the program start - insert at very beginning
                n->next = program;
                program = n;
            }
            else
            {
                // insert n between beforeHeader and header
                n->next = header;
                beforeHeader->next = n;
            }

            // update nodeIndex and nodeList to reflect new position
            // rebuild from scratch to stay accurate for remaining iterations
            nodeIndex.clear();
            nodeList.clear();

            curr = program;
            idx = 0;

            while (curr != nullptr) {nodeIndex[curr] = idx++; nodeList.push_back(curr); curr = curr->next;}
            total = idx;

            // update loop bounds since positions shifted
            loop.header_idx = nodeIndex[header];
            loop.jmp_idx = nodeIndex[jmpNode];
            body_start = loop.header_idx;
            body_end = loop.jmp_idx;

            // recompute written_in_loop and loop_write_count for updated body
            written_in_loop.clear();
            loop_write_count.clear();
            for (int j = body_start;j <= body_end;j++)
            {
                struct InstructionNode* m = nodeList[j];
                if (m->type == ASSIGN)
                {
                    written_in_loop.insert(m->assign_inst.left_hand_side_index);
                    loop_write_count[m->assign_inst.left_hand_side_index]++;
                }
                if (m->type == IN)
                {
                    written_in_loop.insert(m->input_inst.var_index);
                }
                if (m->type == ARRAY_READ)
                {
                    written_in_loop.insert(m->array_inst.target_index);
                }
            }

            // restart body scan from the beginning since positions changed
            i = body_start;
            hoisted++;
        }
    }
    return hoisted;
}
