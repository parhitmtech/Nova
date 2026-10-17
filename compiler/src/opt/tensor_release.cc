/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Compiler-inserted tensor frees (TENSOR_RELEASE) driven by liveness. Split out of compiler.cc.
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

extern unordered_set<InstructionNode*> pure_functions;

// ── Compiler-inserted tensor frees ──────────────────────────────────────────
//
// Nova types tensor handles as plain TYPE_INT (see the tensorToInt coercions
// in parser.cc), so g_tensorSlots — populated during parsing — is the only
// record of which mem[] slots actually hold a tensor handle. Two things
// follow from that:
//
//  1. We can never treat an arbitrary int slot as a tensor slot; every check
//     below is gated on g_tensorSlots membership.
//  2. A slot's tensor-ness can be discovered after the code that would
//     need to propagate it has already been parsed — e.g. a function is
//     defined, then later called with a tensor argument. propagate_tensor_slots()
//     below runs a small fixed-point pass over the whole program, post-parse,
//     to catch that.
//
// Tensor handles alias freely (int b = a; makes two slots reference the same
// tensor_heap entry), so frees can't be placed by liveness alone — that's
// unsound the moment two slots hold the same handle. Instead tensor_heap
// entries are refcounted (compiler.h / alloc_tensor / tensor_retain /
// tensor_release), and liveness is used only to pick where to insert a
// release for each individual slot's own alias, via insert_tensor_releases()
// below. See get_tensor_release_sites() for the two deliberate exceptions
// (a CALL's parameter slots, a RET's own return slot) that keep a function
// call's two sides from releasing a handle out from under each other.
//
// Scope: this does not track tensor handles stored inside arrays, structs, or
// read/printed via OUT — none of the current test suite does that, and
// extending to cover it is a separate piece of work, not silently unsound by
// omission (an untracked slot is just never auto-freed, same as today).

static void propagate_tensor_slots(const std::vector<struct InstructionNode*>& allBodies,
                                   std::set<struct InstructionNode*>& taintedParamFunctions)
{
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (auto bodyHead : allBodies)
        {
            for (struct InstructionNode* n = bodyHead; n != nullptr; n = n->next)
            {
                if (n->type == ASSIGN && n->assign_inst.op ==  OPERATOR_NONE)
                {
                    if (g_tensorSlots.count(n->assign_inst.operand1_index) && 
                        !g_tensorSlots.count(n->assign_inst.left_hand_side_index))
                    {
                        g_tensorSlots.insert(n->assign_inst.left_hand_side_index);
                        changed = true;
                    }
                }
                else if (n->type == CALL)
                {
                    // Same mem[]/fmem[]/dmem[] space caveat as get_tensor_release_sites' CALL case:
                    // only an int-typed (or untyped, defaulting to mem[]) param can possibly be a tensor 
                    // slot, so float/double params are skipped rather than cross-checked against g_tensorSlots.
                    for (int i = 0; i < n->call_inst.num_params; i++)
                    {
                        VarType pt = n->call_inst.param_types ? n->call_inst.param_types[i] : TYPE_UNKNOWN;
                        if (pt == TYPE_FLOAT || pt == TYPE_DOUBLE) continue;
                        int argSlot = n->call_inst.arg_val_slots[i];
                        int paramSlot = n->call_inst.param_slots[i];
                        if (g_tensorSlots.count(argSlot) && !g_tensorSlots.count(paramSlot))
                        {
                            g_tensorSlots.insert(paramSlot);
                            taintedParamFunctions.insert(n->call_inst.function_head);
                            changed = true;
                        }
                    }
                }
            }
        }
    }
}

// Variant of get_defs/get_uses (above) scoped to tensor-release placement —
// not a general-purpose liveness helper, and deliberately different from
// get_defs/get_uses in two ways:
//  - A CALL's parameter slots are not treated as defs here. They belong to
//    the callee's own liveness domain (computed separately, per function),
//    not the caller's; releasing them using the caller's notion of "dead"
//    would release the callee's copy using a stale value, since the callee
//    has already run (and its own RET/body already handled that slot) by
//    the time control returns to the instruction after CALL.
//  - A RET's own return slot is not treated as a use here. Returning a
//    tensor transfers ownership to the caller rather than ending its life;
//    the caller's own CALL-node def of its destination slot is where that
//    handle's remaining life is tracked, via the same uniform rule
static void get_tensor_release_sites(struct InstructionNode* n,
                                        std::unordered_set<int>& defs,
                                        std::unordered_set<int>& uses)
{
    switch (n->type)
    {
        case ASSIGN:
        {
            defs.insert(n->assign_inst.left_hand_side_index);
            uses.insert(n->assign_inst.operand1_index);
            if (n->assign_inst.op != OPERATOR_NONE)
            {
                uses.insert(n->assign_inst.operand2_index);
            }
            break;
        }
        case TENSOR_CALL:
        {
            // result_slot is a mem[] handle/scalar slot only for ops outside
            // TENSOR_DOUBLE_OPS -- see tensor_call_result_is_mem_slot() above
            // (e.g. tensor_get()'s result is a dmem[] scalar, not a handle).
            // arg_slots is likewise NOT uniformly mem[]-space: TEN_SET's
            // value argument (getScalar(3) in execute_program) can be a
            // fmem[]/dmem[] index instead, per arg_types[i]. mem[], fmem[]
            // and dmem[] are independent, separately-numbered spaces, so
            // either one must be skipped unless confirmed mem[]-space, or it
            // can collide by raw number with an unrelated, actually-tensor-
            // tainted mem[] slot.
            if (n->tensor_call_inst.result_slot >= 0 && 
                tensor_call_result_is_mem_slot(n->tensor_call_inst.op))
            {
                defs.insert(n->tensor_call_inst.result_slot);
            }
            for (int i = 0;i < n->tensor_call_inst.num_args; i++)
            {
                VarType at = n->tensor_call_inst.arg_types[i];
                if (at == TYPE_FLOAT || at == TYPE_DOUBLE) continue;
                uses.insert(n->tensor_call_inst.arg_slots[i]);
            }
            break;
        }
        case CALL:
        {
            // mem[], fmem[] and dmem[] are three independent slot spaces,
            // each numbered from its own counter -- so "slot 3" in fmem[] or
            // dmem[] has nothing to do with "slot 3" in mem[], and g_tensorSlots
            // (which only ever stores mem[]-space indices) must not be
            // consulted using a slot number taken from the wrong space.
            // CALL's param_slots/arg_val_slots carry a parallel param_types/
            // arg_types array precisely because a call can bind any mix of
            // the three; TYPE_UNKNOWN (no type info) defaults to mem[], same
            // as the interpreter's own fallback in its param-binding loop.
            defs.insert(n->call_inst.ret_val_index);
            if (n->call_inst.all_ret_slots)
            {
                for (int i = 0;i < n->call_inst.num_ret_slots; i++)
                {
                    defs.insert(n->call_inst.all_ret_slots[i]);
                }
            }
            for (int i = 0;i < n->call_inst.num_params; i++)
            {
                VarType pt = n->call_inst.param_types ? n->call_inst.param_types[i] : TYPE_UNKNOWN;
                if (pt == TYPE_FLOAT || pt == TYPE_DOUBLE) continue;
                uses.insert(n->call_inst.arg_val_slots[i]);
            }
            break;
        }
        case CJMP:
        {
            if (n->cjmp_inst.operand_type != TYPE_FLOAT && n->cjmp_inst.operand_type != TYPE_DOUBLE)
            {
                uses.insert(n->cjmp_inst.operand1_index);
                uses.insert(n->cjmp_inst.operand2_index);
            }
            break;
        }
        // RET deliberately has no case: its return slot(s) must not be released here 
        default: break;
    }
}

// Object slots hold object state and are never released by liveness; see g_fieldSlots / g_selfSlots
// in parser.cc and own_kind in compiler.h
static bool is_object_slot(int s)
{
    return g_fieldSlots.count(s)  || g_selfSlots.count(s);
}

static void insert_tensor_releases_in_body(struct InstructionNode* bodyHead)
{
    if (bodyHead == nullptr || g_tensorSlots.empty()) return ;
    LivenessResult lr = compute_liveness(bodyHead);

    // Snapshot nodes before splicing new ones in.
    std::vector<struct InstructionNode*> nodes;
    for (struct InstructionNode* n = bodyHead; n != nullptr; n = n->next)
    {
        nodes.push_back(n);
    }
    // Branch edges: the per-node rule only releases right after a last def/use. If the last
    // use is inside one arm of an if, the path that skips the arm never releases it (one leak per
    // run of that path).
    // Only release on an edge a slot that is definitely written on every path to the branch: a
    // never-written slot still holds mem[]'s initial 0, and handle 0 is a real tensor. Forward "must"
    // analysis: assigned-in(n) = intersection over preds; assigned-out = in + defs(n).
    std::unordered_map<struct InstructionNode*, std::vector<struct InstructionNode*>> preds;
    for (auto n : nodes)
    {
        std::vector<struct InstructionNode*> succs;
        get_successors(n, succs);
        for (auto sx : succs) preds[sx].push_back(n);
    }
    std::unordered_map<struct InstructionNode*, std::unordered_set<int>> assignedOut;
    std::unordered_set<struct InstructionNode*> computed;
    bool daChanged = true;
    while (daChanged)
    {
        daChanged = false;
        for (auto n : nodes)
        {
            std::unordered_set<int> in;
            if (n != bodyHead)
            {
                bool first = true;
                for (auto p : preds[n])
                {
                    if (!computed.count(p)) continue;  // not visited yet = "everything"
                    if (first) { in = assignedOut[p]; first = false; }
                    else 
                    {
                        std::unordered_set<int> keep;
                        for (int x : in) if (assignedOut[p].count(x)) keep.insert(x);
                        in.swap(keep);
                    }
                }
            }
            std::unordered_set<int> d, u;
            get_tensor_release_sites(n, d, u);
            for (int x : d) in.insert(x);
            if (!computed.count(n) || in != assignedOut[n])
            {
                assignedOut[n] = in;
                computed.insert(n);
                daChanged = true;
            }
        }
    }

    // Plan edge releases from the original successors (applied at the end).
    struct EdgePlan { struct InstructionNode* cjmp; struct InstructionNode* fallnext;
                      struct InstructionNode* jumpTarget;
                      std::vector<int> fallRel, jumpRel; };
    std::vector<EdgePlan> edgePlans;
    for (auto n : nodes)
    {
        if (n->type != CJMP || n->next == nullptr || n->cjmp_inst.target == nullptr) continue;
        EdgePlan ep{n, n->next, n->cjmp_inst.target, {}, {}};
        const auto& out = lr.live_out[n];
        const auto& inFall = lr.live_in[n->next];
        const auto& inJump = lr.live_in[n->cjmp_inst.target];
        const auto& assigned = assignedOut[n];
        for (int s : out)
        {
            if (!g_tensorSlots.count(s) || is_object_slot(s) || !assigned.count(s)) continue;
            if (!inFall.count(s)) ep.fallRel.push_back(s);  // dies on fall-through
            if (!inJump.count(s)) ep.jumpRel.push_back(s);  // dies on the jump
        }
        if (!ep.fallRel.empty() || !ep.jumpRel.empty()) edgePlans.push_back(ep);
    }

    // Per-node rule (unchanged): release right after a slot's last def/use.
    for (auto n : nodes)
    {
        std::unordered_set<int> defs, uses;
        get_tensor_release_sites(n, defs, uses);
        const auto& liveOut = lr.live_out[n];

        std::vector<int> toRelease;
        for (int s : defs)
        {
            if (g_tensorSlots.count(s) && !is_object_slot(s) && !liveOut.count(s))
            {
                toRelease.push_back(s);
            }
        }
        for (int s : uses)
        {
            if (g_tensorSlots.count(s) && !is_object_slot(s) && !liveOut.count(s) && 
                std::find(toRelease.begin(), toRelease.end(), s) == toRelease.end())
            {
                toRelease.push_back(s);
            }
        }
        for (int s : toRelease)
        {
            struct InstructionNode* rel = new InstructionNode();
            rel->line_no = n->line_no;
            rel->type = TENSOR_RELEASE;
            rel->tensor_release_inst.slot = s;
            rel->next = n->next;
            n->next = rel;
            n = rel;  // chain further releases at this same point after it
        }
    }

    // Apply edge releases.
    //   before: CJMP(target=T) -> N
    //   after: CJMP(target=TR) -> [fall releases] -> JMP N -> TR:[jump releases] -> JMP T -> N
    auto mkRelease = [](int slot, int line) {
        struct InstructionNode* r = new InstructionNode();
        r->line_no = line; r->type = TENSOR_RELEASE; r->tensor_release_inst.slot = slot;
        return r;
    };
    auto mkJmp = [](struct InstructionNode* target, int line) {
        struct InstructionNode* j = new InstructionNode();
        j->line_no = line; j->type = JMP; j->jmp_inst.target = target;
        return j;
    };
    for (auto& ep : edgePlans)
    {
        struct InstructionNode* c = ep.cjmp;
        int line = c->line_no;
        struct InstructionNode* tail = c;  // end of the fall-through chain
        struct InstructionNode* after = c->next;  // keep whatever follows the CJMP
        for (int s : ep.fallRel) { auto r = mkRelease(s, line); tail->next = r; tail = r; }
        if (!ep.jumpRel.empty())
        {
            auto skip = mkJmp(ep.fallnext, line);  // fall-through hops over the detour
            tail->next = skip; tail = skip;
            struct InstructionNode* first = nullptr;
            for (int s : ep.jumpRel)
            {
                auto r = mkRelease(s, line);
                if (!first) first = r;
                tail->next = r; tail = r;
            }
            auto back = mkJmp(ep.jumpTarget, line);
            tail->next = back; tail = back;
            c->cjmp_inst.target = first;  // the jump now goes through the detour
        }
        tail->next = after;
    }
}

// Entry point: run after parsing (and after any --optimize passes) completes,
// so this sees the final IR that will actually execute. Also strips any
// function whose parameter was found to be tensor-tainted out of
// pure_functions, so it can never be selected for memoization — the memo
// cache keys on raw argument values, and a tensor handle is reused (by
// value) once freed, so memoizing a call keyed on a handle that may later be
// released and reassigned would be a stale-cache-hit bug waiting to happen.
void insert_tensor_releases(struct InstructionNode* program)
{
    std::vector<struct InstructionNode*> allBodies;
    std::set<struct InstructionNode*> seen;
    for (auto& kv : functionTable)
    {
        if (kv.second != nullptr && seen.insert(kv.second).second) allBodies.push_back(kv.second);
    }
    // class-method bodies too (they used to get no tensor frees at all)
    for (auto& ckv : classMethodTable)
    {
        for (auto& mkv : ckv.second)
        {
            if (mkv.second != nullptr && seen.insert(mkv.second).second) allBodies.push_back(mkv.second);
        }
    }
    allBodies.push_back(program);

    std::set<struct InstructionNode*> taintedParamFunctions;
    propagate_tensor_slots(allBodies, taintedParamFunctions);

    // Mark ASSIGNs that write object state (see own_kind in compiler.h):
    // self <- field and field <- self are the method copy-in / copy-back
    // (move); any other write into a field or self slot is a store.
    for (auto bodyHead : allBodies)
    {
        for (struct InstructionNode* n = bodyHead; n != nullptr; n = n->next)
        {
            if (n->type != ASSIGN || n->assign_inst.op != OPERATOR_NONE) continue;
            int lhs = n->assign_inst.left_hand_side_index;
            int src = n->assign_inst.operand1_index;
            bool move = (g_selfSlots.count(lhs) && g_fieldSlots.count(src)) || 
                        (g_fieldSlots.count(lhs) && g_selfSlots.count(src));
            if (move) n->assign_inst.own_kind = 2;
            else if (is_object_slot(lhs)) n->assign_inst.own_kind = 1;
        }
    }

    for (auto fhead : taintedParamFunctions)
    {
        pure_functions.erase(fhead);
    }
    for (auto bodyHead : allBodies)
    {
        insert_tensor_releases_in_body(bodyHead);
    }
}
