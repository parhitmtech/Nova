/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Interference graph, Chaitin-Briggs coloring, constant slots and spill code. Split out of compiler.cc.
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

// x86-64 registers available for allocation. rax (return value / scratch), rsp and rbp are reserved.
const vector<string> REGISTERS = {
    "rbx", "r12", "r13", "r14", "r15",  // callee-saved - safe across calls
    "rcx", "rdx", "rsi", "rdi",  // caller-saved - clobbered by calls
    "r8", "r9", "r10", "r11"  // caller-saved
};
const int K = (int)REGISTERS.size();  // number of available registers

unordered_map<InstructionNode*, ColoringResult> g_funcColorings;

// ── Interference Graph ────────────────────────────────────────────────────────

InterferenceGraph build_interference_graph(
    struct InstructionNode* program, 
    const LivenessResult& lr)
{
    InterferenceGraph ig;

    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        // Add all live slots as nodes
        for (int s : lr.live_in.at(pc)) ig.add_node(s);
        for (int s : lr.live_out.at(pc)) ig.add_node(s);

        // Get defs for this instruction
        unordered_set<int> defs;
        get_defs(pc, defs);

        // For each def, it interferes with everything in live_out
        // Because def is written here, and live_out slots are needed after that
        // so they cannot share a register
        for (int d : defs)
        {
            ig.add_node(d);
            for (int live : lr.live_out.at(pc))
            {
                ig.add_edge(d, live);
            }
        }

        // Special case: ASSIGN (copy instructions) a = b
        // In standard Briggs/Chaitin, copy-rellated slots are NOT added as
        // interfering here (enables coalescing later)
        // For now we add them anyway - coalescing is optional
        
        pc = pc->next;
    }
    return ig;
}

void dump_interference_graph(const InterferenceGraph&) {}

// ── Graph Coloring (Chaintin-Briggs) ────────────────────────────────────────────────────────

// Find all slots that are live across a CALL instruction
unordered_set<int> find_live_across_calls(
    struct InstructionNode* program,
    const LivenessResult& lr)
{
    unordered_set<int> result;
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == CALL)
        {
            // Everything live after a call must survive the call
            for (int slot: lr.live_out.at(pc))
            {
                result.insert(slot);
            }
        }
        pc = pc->next;
    }
    return result;
}

ColoringResult color_graph(InterferenceGraph ig, 
                           const unordered_set<int>& liveAcrossCallSlots,
                           const unordered_set<int>& constantSlots)
{
    ColoringResult result;

    // Add interference edges between ALL pairs of constants
    // since they are all live simultaneously at function entry
    for (int a : constantSlots)
    {
        for (int b : constantSlots)
        {
            if (a != b) ig.add_edge(a, b);
        }
    }

    // Constants are live throughout the entire function
    // so they interfere with ALL other slots in the graph
    for (int a : constantSlots)
    {
        for (auto& kv : ig.adj)
        {
            if (!constantSlots.count(kv.first))
            {
                ig.add_edge(a, kv.first);
            }
        }
    }

    // Work on a copy of adjacency so we can remove nodes
    unordered_map<int, unordered_set<int>> adj = ig.adj;

    // Pre-color constants using sorted order for determinism
    // Constants are re-initialized at function entry so they DON'T need callee-saved 
    // Use caller-saved registers (indices 5+) for constants to free up callee-saved
    vector<int> sortedConsts(constantSlots.begin(), constantSlots.end());
    sort(sortedConsts.begin(), sortedConsts.end());

    // Pre-color constant slots
    for (int i = 0; i < (int)sortedConsts.size(); i++)
    {
        int s = sortedConsts[i];
        ig.add_node(s);
        unordered_set<int> usedColors;

        // Colors used by neighbors in the graph
        for (int nb : ig.adj[s])
            if (result.color.count(nb)) usedColors.insert(result.color[nb]);

        // Colors already used by previously colored constants (indices 0..i-1)
        for (int j = 0; j < i; j++)
            if (result.color.count(sortedConsts[j]))
                usedColors.insert(result.color[sortedConsts[j]]);

        // Start from index 5 (caller-saved) for constants
        // since constants are re-initialized at entry, no need for callee-saved
        for (int c = 5; c < K; c++)
            if (!usedColors.count(c)) { result.color[s] = c; break; }
    }

    // Track which nodes are still active
    vector<int> stack;
    unordered_set<int> active;
    for (auto& kv : adj) active.insert(kv.first);

    // Phase 1: Simplify
    // Repeatedly remove nodes with degree < K
    // If no such node exists, spill the highest-degree node
    while (!active.empty())
    {
        // Find a node with degree < K
        int chosen = -1;
        for (int node : active)
        {
            // Count active neighbors only
            int activeDegree = 0;
            for (int neighbor : adj[node])
            {
                if (active.count(neighbor)) activeDegree++;
            }
            if (activeDegree < K)
            {
                chosen = node;
                break;
            }
        }

        if (chosen == -1)
        {
            // No node with degree < K - must spill
            int maxDegree = -1;
            for (int node : active)
            {
                int activeDegree = 0;
                for (int neighbor : adj[node])
                {
                    if (active.count(neighbor)) activeDegree++;
                }
                if (activeDegree > maxDegree)
                {
                    maxDegree = activeDegree;
                    chosen = node;
                }
            }
            result.spilled.insert(chosen);
        }

        // Remove chosen from active and push onto stack
        active.erase(chosen);
        stack.push_back(chosen);
    }

    // Phase 2: Select
    // Pop nodes off stack and assign colors
    while (!stack.empty())
    {
        int node = stack.back();
        stack.pop_back();

        if (result.spilled.count(node)) continue; // skip spilled nodes
        if (result.color.count(node)) continue; // already pre-colored, skip

        // Find colors used by neighbors
        unordered_set<int> usedColors;
        for (int neighbor : adj[node])
        {
            if (result.color.count(neighbor))
            {
                usedColors.insert(result.color[neighbor]);
            }
        }

        // Check if this slot is live across any call
        // If so, only use callee-saved registers (indices 0-4)
        bool liveAcrossCall = liveAcrossCallSlots.count(node) > 0;

        // Try callee-saved first (indices 0-4)
        bool colored = false;
        int limit = liveAcrossCall ? 5 : K;
        for (int c = 0;c < limit;c++)
        {
            if (!usedColors.count(c))
            {
                result.color[node] = c;
                colored = true;
                break;
            }
        }
        
        // If couldn't fit in callee-saved, try caller-saved
        if (!colored)
        {
            for (int c = limit; c < K;c++)
            {
                if (!usedColors.count(c))
                {
                    result.color[node] = c;
                    colored = true;
                    break;
                }
            }
        }

        if (!colored)
        {
            result.spilled.insert(node);
        }
    }
    return result;
}

void dump_coloring(const ColoringResult&) {}

// x86 Assembly generation

// Helper to find slots that are constants (written at parse time, never by IR)
unordered_set<int> find_constant_slots(struct InstructionNode* program)
{
    unordered_set<int> written_by_ir;
    unordered_set<int> used_by_ir;

    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        // Collect defs
        unordered_set<int> defs;
        get_defs(pc, defs);
        for (int d : defs) written_by_ir.insert(d);

        // Collect uses
        unordered_set<int> uses;
        get_uses(pc, uses);
        for (int u : uses) used_by_ir.insert(u);

        pc = pc->next;
    }

    // Constant slots: non-zero mem[] value, never written by IR, BUT used by IR
    unordered_set<int> constants;
    for (int i = 0; i < next_available; i++)
    {
        if (mem[i] != 0 && !written_by_ir.count(i) && used_by_ir.count(i))
        {
            constants.insert(i);
        }
    }
    return constants;
}

// Insert spill loads and stores around instructions that use spilled slots
struct InstructionNode* insert_spill_code(struct InstructionNode* program, const ColoringResult& cr)
{
    if (cr.spilled.empty()) return program;  // nothing to do, no spill handling

    // For each spilled slot, allocate a fresh temp slot for it
    unordered_map<int, int> spillTemp;  // spilled slot -> temp slot
    for (int s : cr.spilled)
    {
        int temp = alloc_slot();
        spillTemp[s] = temp;
    }

    // Helper to create a SPILL_LOAD node
    auto makeLoad = [&](int spillSlot, int tempSlot) -> InstructionNode*
    {
        InstructionNode* n = new InstructionNode();
        n->type = SPILL_LOAD;
        n->line_no = 0;
        n->next = nullptr;
        n->spill_inst.spill_slot = spillSlot;
        n->spill_inst.temp_slot = tempSlot;
        return n;
    };

    // Helper to create a SPILL_STORE node
    auto makeStore = [&](int spillSlot, int tempSlot) -> InstructionNode*
    {
        InstructionNode* n = new InstructionNode();
        n->type = SPILL_STORE;
        n->line_no = 0;
        n->next = nullptr;
        n->spill_inst.spill_slot = spillSlot;
        n->spill_inst.temp_slot = tempSlot;
        return n;
    };

    // Helper to rewrite a slot index - if spilled, replace with temp slot
    auto rewrite = [&](int slot) -> int
    {
        if (spillTemp.count(slot)) return spillTemp[slot];
        return slot;
    };

    // Walk the IR and insert loads/stores
    struct InstructionNode* pc = program;
    struct InstructionNode* prev = nullptr;

    while (pc != nullptr)
    {
        struct InstructionNode* next = pc->next;

        // Find uses of spilled slots - insert SPILL_LOAD before this node
        unordered_set<int> uses, defs;
        get_uses(pc, uses);
        get_defs(pc, defs);

        // Insert loads for spilled slots that are used by this instruction
        struct InstructionNode* insertBefore = nullptr;  // chain of loads to insert
        struct InstructionNode* insertBeforeTail = nullptr; 

        for (int u : uses)
        {
            if (!cr.spilled.count(u)) continue;
            int temp = spillTemp[u];
            InstructionNode* load = makeLoad(u, temp);
            if (!insertBefore) insertBefore = insertBeforeTail = load;
            else { insertBeforeTail->next = load; insertBeforeTail = load; }
        }   

        // Rewrite the instruction itself to use temp slots instead of spilled slots
        switch (pc->type)
        {
            case ASSIGN:
            {
                pc->assign_inst.operand1_index = rewrite(pc->assign_inst.operand1_index);
                if (pc->assign_inst.op != OPERATOR_NONE)
                {
                    pc->assign_inst.operand2_index = rewrite(pc->assign_inst.operand2_index);
                }
                pc->assign_inst.left_hand_side_index = rewrite(pc->assign_inst.left_hand_side_index);
                break;
            }
            case OUT:
            {
                pc->output_inst.var_index = rewrite(pc->output_inst.var_index);
                break;
            }
            case CJMP:
            {
                pc->cjmp_inst.operand1_index = rewrite(pc->cjmp_inst.operand1_index);
                pc->cjmp_inst.operand2_index = rewrite(pc->cjmp_inst.operand2_index);
                break;
            }
            case IN:
            {
                pc->input_inst.var_index = rewrite(pc->input_inst.var_index);
                break;
            }
            case CALL:
            {
                for (int i = 0;i < pc->call_inst.num_params;i++)
                {
                    pc->call_inst.arg_val_slots[i] = rewrite(pc->call_inst.arg_val_slots[i]);
                }
                // The result is a def: write it to the temp, which the SPILL_STORE inserted after
                // this node copies back. (Leaving it unrewritten let that store overwrite the
                // result with the stale temp.)
                pc->call_inst.ret_val_index = rewrite(pc->call_inst.ret_val_index);
                for (int i = 0; pc->call_inst.all_ret_slots && i < pc->call_inst.num_ret_slots; i++)
                    pc->call_inst.all_ret_slots[i] = rewrite(pc->call_inst.all_ret_slots[i]);
                break;
            }
            case RET:
            {
                pc->ret_inst.ret_val_index = rewrite(pc->ret_inst.ret_val_index);
                break;                
            }
            case ARRAY_READ:
            {
                pc->array_inst.index_slot = rewrite(pc->array_inst.index_slot);
                pc->array_inst.target_index = rewrite(pc->array_inst.target_index);
                if (pc->array_inst.dynamic_base)
                {
                    pc->array_inst.base_index = rewrite(pc->array_inst.base_index);
                }
                break;
            }
            case ARRAY_WRITE:
            {
                pc->array_inst.index_slot = rewrite(pc->array_inst.index_slot);
                pc->array_inst.target_index = rewrite(pc->array_inst.target_index);
                if (pc->array_inst.dynamic_base)
                {
                    pc->array_inst.base_index = rewrite(pc->array_inst.base_index);
                }
                break;
            }
            default: break;
        }

        // Insert SPILL_STORE after this node for any spilled slots that are DEFINED
        struct InstructionNode* insertAfter = nullptr;
        struct InstructionNode* insertAfterTail = nullptr;

        for (int d : defs)
        {
            if (!cr.spilled.count(d)) continue;
            int temp = spillTemp[d];
            InstructionNode* store = makeStore(d, temp);
            if (!insertAfter) insertAfter = insertAfterTail = store;
            else { insertAfterTail->next = store; insertAfterTail = store; }
        }

        // Wire everything together;
        // prev -> [loads] -> pc -> [stores] -> next
        if (insertBefore)
        {
            insertBeforeTail->next = pc;
            if (prev) prev->next = insertBefore;
            else program = insertBefore;
        }

        if (insertAfter)
        {
            insertAfterTail->next = next;
            pc->next = insertAfter;
            prev = insertAfterTail;
        }
        else
        {
            prev = pc;
        }

        pc = pc->next;
    }
    return program;
}
