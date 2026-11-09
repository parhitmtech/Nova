// Optimizer: loop-invariant code motion. An int computation inside a loop whose operands the loop never writes
// (i * 4 in a loop over j) moves to just before the loop, so it runs once instead of once per iteration. Inner loops
// go first, so a computation can move out of several loops in turn.
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "compiler.h"
#include "optimizer.h"
#include "regalloc.h"

using namespace std;

namespace
{
    vector<InstructionNode*> nodes_of(InstructionNode* program)
    {
        vector<InstructionNode*> nodes;
        for (InstructionNode* n = program; n != nullptr; n = n->next) nodes.push_back(n);
        return nodes;
    }

    // Whether n can move to just before the loop nodes[start..end] (start: the header, the back edge's target).
    // It must compute the same value on every iteration, and moving it must not change what the program does:
    // - an int + - or * (a division could stop the program on a path that never ran it), of operands the loop
    //   never writes, into a slot the loop writes only here;
    // - its slot is not live at the header, so nothing in the loop reads the slot before n writes it, and not read
    //   outside the loop, so a loop that runs no times leaves the program as it was;
    // - the header is reached from outside the loop only by falling through from the node before it, so the moved
    //   node runs on every way in.
    bool can_hoist(const vector<InstructionNode*>& nodes, int start, int end, InstructionNode* n,
                   const LivenessResult& live)
    {
        if (n->type != ASSIGN) return false;
        ArithmeticOperatorType op = n->assign_inst.op;
        if (op != OPERATOR_PLUS && op != OPERATOR_MINUS && op != OPERATOR_MULT) return false;
        int dst = n->assign_inst.left_hand_side_index;
        int writes = 0;
        for (int i = start; i <= end; i++)
        {
            unordered_set<int> defs;
            get_defs(nodes[i], defs);
            if (defs.count(n->assign_inst.operand1_index) || defs.count(n->assign_inst.operand2_index)) return false;
            writes += (int)defs.count(dst);
        }
        if (writes != 1 || live.live_in.at(nodes[start]).count(dst)) return false;
        if (start == 0 || nodes[start - 1]->type == JMP) return false;
        for (int i = 0; i < (int)nodes.size(); i++)
        {
            InstructionNode* m = nodes[i];
            // n is reached only by falling into it, so taking it out leaves no jump pointing at it
            if ((m->type == JMP && m->jmp_inst.target == n) || (m->type == CJMP && m->cjmp_inst.target == n))
            {
                return false;
            }
            if (i >= start && i <= end) continue;
            unordered_set<int> uses;
            get_uses(m, uses);
            if (uses.count(dst)) return false;
            vector<InstructionNode*> succs;
            get_successors(m, succs);
            bool entersHeader = find(succs.begin(), succs.end(), nodes[start]) != succs.end();
            if (entersHeader && i != start - 1) return false;
        }
        return true;
    }
}

int loop_invariant_code_motion(struct InstructionNode* program)
{
    // The loops: each back edge, a JMP to an earlier node (the loop's header)
    vector<InstructionNode*> nodes = nodes_of(program);
    unordered_map<InstructionNode*, int> index;
    for (int i = 0; i < (int)nodes.size(); i++) index[nodes[i]] = i;
    vector<pair<InstructionNode*, InstructionNode*>> loops;  // header, back edge
    for (int i = 0; i < (int)nodes.size(); i++)
    {
        InstructionNode* target = nodes[i]->type == JMP ? nodes[i]->jmp_inst.target : nullptr;
        if (target != nullptr && index.count(target) && index[target] < i) loops.push_back({target, nodes[i]});
    }
    // innermost first: the latest header
    sort(loops.begin(), loops.end(), [&](const auto& a, const auto& b) { return index[a.first] > index[b.first]; });

    int hoisted = 0;
    for (const auto& [header, backEdge] : loops)
    {
        bool moved = true;
        while (moved)  // the positions change with each move: look again from the start
        {
            moved = false;
            nodes = nodes_of(program);
            index.clear();
            for (int i = 0; i < (int)nodes.size(); i++) index[nodes[i]] = i;
            int start = index[header], end = index[backEdge];
            LivenessResult live = compute_liveness(program);
            for (int i = start + 1; i < end && !moved; i++)
            {
                InstructionNode* n = nodes[i];
                if (!can_hoist(nodes, start, end, n, live)) continue;
                nodes[i - 1]->next = n->next;      // out of the loop ...
                n->next = header;                  // ... and in just before its header
                nodes[start - 1]->next = n;
                hoisted++;
                moved = true;
            }
        }
    }
    return hoisted;
}
