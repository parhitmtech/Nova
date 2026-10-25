// Interpreter: function calls (memoization, JIT fast path) and returns. Split out of execute_program (interpreter.cc).
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include <stack>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "lexer.h"
#include "compiler.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"
#include "tensor_ops.h"
#include "interp_internal.h"

using namespace std;

// A virtual call (a method call): the same call aimed at the method the object's own class runs, so an
// override runs when the object belongs to a subclass. One copy per call site and class, made the first time.
static InstructionNode* virtual_call_target(InstructionNode* pc)
{
    int cls = objects_class_of(mem[pc->call_inst.arg_val_slots[0]], pc->line_no);
    static map<pair<InstructionNode*, int>, InstructionNode*> copies;
    InstructionNode*& copy = copies[{pc, cls}];
    if (copy) return copy;
    auto c = virtualTable.find(cls);
    if (c == virtualTable.end() || !c->second.count(pc->call_inst.virtual_method))
    {
        tensor_shape_error(pc->line_no, "the object's class has no such method");
    }
    const MethodTarget& t = c->second[pc->call_inst.virtual_method];
    copy = new InstructionNode(*pc);
    copy->call_inst.function_head = t.head;
    copy->call_inst.param_slots = t.param_slots;
    copy->call_inst.param_types = t.param_types;
    copy->call_inst.func_slot_base = t.slot_base;
    copy->call_inst.func_slot_count = t.slot_count;
    copy->call_inst.virtual_method = 0;
    return copy;
}

InstructionNode* exec_call(InstructionNode* pc)
{
    if (pc->call_inst.virtual_method) pc = virtual_call_target(pc); // its next is the call site's next
    do {
            if (pc->call_inst.function_head == NULL)
            {
                debug("Error: CALL target function_head is null.\n");
                exit(1);
            }

            InstructionNode* fhead = pc->call_inst.function_head;

            // Memoization fast path: pure + recursive functions cache arg→result
            bool is_memo = pure_functions.count(fhead) && recursive_functions.count(fhead);
            vector<int> memo_key;
            if (is_memo)
            {
                for (int i = 0; i < pc->call_inst.num_params; i++)
                    memo_key.push_back(mem[pc->call_inst.arg_val_slots[i]]);
                auto fit = memo_cache.find(fhead);
                if (fit != memo_cache.end())
                {
                    auto kit = fit->second.find(memo_key);
                    if (kit != fit->second.end())
                    {
                        const vector<int>& retVals = kit->second;
                        if (!retVals.empty())
                        {
                            mem[pc->call_inst.ret_val_index] = retVals[0];
                            if (pc->call_inst.all_ret_slots)
                            {
                                for (int i = 0; i < (int)retVals.size() && i < pc->call_inst.num_ret_slots; i++)
                                    mem[pc->call_inst.all_ret_slots[i]] = retVals[i];
                            }
                        }
                        pc = pc->next;
                        break;
                    }
                }
            }

            // Interpreter fallback path
            CallFrame frame;
            frame.returnAddress  = pc->next;
            frame.dest_index     = pc->call_inst.ret_val_index;
            frame.dest_type      = pc->call_inst.ret_type;
            frame.slot_base      = pc->call_inst.func_slot_base;
            frame.is_memoizable  = is_memo;
            frame.memo_func      = is_memo ? fhead : nullptr;
            frame.memo_key       = memo_key;

            if (pc->call_inst.all_ret_slots)
            {
                frame.allRetSlots = vector<int>(
                    pc->call_inst.all_ret_slots,
                    pc->call_inst.all_ret_slots + pc->call_inst.num_ret_slots
                );
                if (pc->call_inst.ret_slot_types)
                {
                    frame.allRetTypes = vector<VarType>(
                        pc->call_inst.ret_slot_types,
                        pc->call_inst.ret_slot_types + pc->call_inst.num_ret_slots
                    );
                }
            }

            int base  = pc->call_inst.func_slot_base;
            int count = pc->call_inst.func_slot_count;
            for (int i = 0; i < count; i++)
                frame.savedSlots.push_back(mem[base + i]);

            objects_enter_call();  // before the parameters: an object parameter is one of the call's references
            for (int i = 0; i < pc->call_inst.num_params; i++)
            {
                int pSlot = pc->call_inst.param_slots[i];
                int aSlot = pc->call_inst.arg_val_slots[i];
                VarType pt = (pc->call_inst.param_types && i < pc->call_inst.num_params)
                            ? pc->call_inst.param_types[i] : TYPE_UNKNOWN;
                VarType at = (pc->call_inst.arg_types) ? pc->call_inst.arg_types[i] : pt;
                double val = 0.0;
                if (at == TYPE_FLOAT)  val = (double)fmem[aSlot];
                else if (at == TYPE_DOUBLE) val = dmem[aSlot];
                else val = (double)mem[aSlot];
                if (pt == TYPE_FLOAT) fmem[pSlot] = (float)val;
                else if (pt == TYPE_DOUBLE) dmem[pSlot] = val;
                else mem[pSlot]  = (int)val;
                // Binding a tensor-handle argument into a param slot creates a second
                // simultaneous alias (caller's aSLot, callee's pSlot) to the same tensor_heap 
                // entry -- unlike RET, whc=ich is a transfer, not a duplicate. Retain here so the 
                // two sides' independent TENSOR_RELEASE insertions (see insert_tensor_releases) each
                // own one reference instead of double-freeing a single one.
                if (g_tensorSlots.count(pSlot)) tensor_retain(mem[pSlot]);
                if (pt == TYPE_CLASS || pt == TYPE_VECTOR || pt == TYPE_MAP) objects_bind_param(pSlot, mem[pSlot], pt);
            }

            callStack.push(frame);
            pc = pc->call_inst.function_head;
            break;
    } while (0);
    return pc;
}

InstructionNode* exec_ret(InstructionNode* pc)
{
    do {
            if (callStack.empty())
            {
                debug("Error: RET with empty call stack.\n");
                exit(1);
            }
            CallFrame frame = callStack.top();
            callStack.pop();

            // capture return value before restoring slots
            // int ret_val = mem[pc->ret_inst.ret_val_index];

            // restore function-local slots to their pre-call  values
            // for (int i = 0; i < (int)frame.savedSlots.size(); i++)
            //     mem[frame.slot_base + i] = frame.savedSlots[i];

            // write return value - caller reads this in the very next ASSIGN
            // mem[frame.dest_index] = ret_val;

            // capture all return values before restore
            // vector<int> capturedRetVals;
            // for (int slot : frame.allRetSlots)
            // {
            //     capturedRetVals.push_back(mem[slot]);
            // }
            // if single return, fall back to ret_inst
            // if (capturedRetVals.empty())
            // {
            //     capturedRetVals.push_back(mem[pc->ret_inst.ret_val_index]);
            // }

            // Populate memo cache for pure recursive functions on first computation
            // if (frame.is_memoizable && frame.memo_func)
            // {
            //     memo_cache[frame.memo_func][frame.memo_key] = capturedRetVals;
            // }

            // restore function-local slots
            // for (int i = 0;i < (int)frame.savedSlots.size();i++)
            // {
            //     mem[frame.slot_base + i] = frame.savedSlots[i];
            // }

            // write all return values back
            // for (int i = 0;i < (int)capturedRetVals.size() && i < (int)frame.allRetSlots.size();i++)
            // {
            //     mem[frame.allRetSlots[i]] = capturedRetVals[i];
            // }
            // //backward compatibility - also to dst_index
            // mem[frame.dest_index] = capturedRetVals[0];

            // Return slots can liev in mem[], fmem[], or dmem[]. Read and write each through 
            // its own array. Old code assumed mem[] for all. double is the common carry type
            // for all three
            vector<double> capturedRetVals;
            for (int i = 0;i < (int)frame.allRetSlots.size();i++)
            {
                int slot = frame.allRetSlots[i];
                VarType rt = (i < (int)frame.allRetTypes.size()) ? frame.allRetTypes[i] : TYPE_UNKNOWN;
                double v = (rt == TYPE_FLOAT) ? (double)fmem[slot] :
                            (rt == TYPE_DOUBLE) ? dmem[slot] : (double)mem[slot];
                capturedRetVals.push_back(v);
            }
            // single return: ues the RET node's own slot and type
            if (capturedRetVals.empty())
            {
                int slot = pc->ret_inst.ret_val_index;
                VarType rt = pc->ret_inst.ret_type;
                double v = (rt == TYPE_FLOAT) ? (double)fmem[slot] :
                           (rt == TYPE_DOUBLE) ? dmem[slot] : (double)mem[slot];
                capturedRetVals.push_back(v);
            }

            // The call's object references go; an object it returns is handed to the caller first
            VarType rt = frame.dest_type;
            bool reference = (rt == TYPE_CLASS || rt == TYPE_VECTOR || rt == TYPE_MAP);
            objects_exit_call(reference ? (int)capturedRetVals[0] : 0, rt);

            // memo cache stays int-only (safe: edit 2 keeps non-int functions out)
            if (frame.is_memoizable && frame.memo_func)
            {
                vector<int> intRetVals;
                for (double v : capturedRetVals) intRetVals.push_back((int)v);
                memo_cache[frame.memo_func][frame.memo_key] = intRetVals;
            }

            // restore function-local slots (mem[] only, unchanged)
            for (int i = 0;i < (int)frame.savedSlots.size();i++)
            {
                mem[frame.slot_base + i] = frame.savedSlots[i];
            }

            // write all return values back, each through its own array
            for (int i = 0;i < (int)capturedRetVals.size() && i < (int)frame.allRetSlots.size();i++)
            {
                int slot = frame.allRetSlots[i];
                VarType rt = (i < (int)frame.allRetTypes.size()) ? frame.allRetTypes[i] : TYPE_UNKNOWN;
                if (rt == TYPE_FLOAT) fmem[slot] = (float)capturedRetVals[i];
                else if (rt == TYPE_DOUBLE) dmem[slot] = capturedRetVals[i];
                else mem[slot] = (int)capturedRetVals[i];
            }

            // also write dest_index, through its own array
            if (frame.dest_type == TYPE_FLOAT) fmem[frame.dest_index] = (float)capturedRetVals[0];
            else if (frame.dest_type == TYPE_DOUBLE) dmem[frame.dest_index] = capturedRetVals[0];
            else mem[frame.dest_index] = (int)capturedRetVals[0];

            // The function's local slots stay its own: they were assigned when it was compiled, and its next call
            // uses them again. (They used to go on freeList here, so a later ALLOC could hand them out as an array.)
            pc = frame.returnAddress;
            break;
    } while (0);
    return pc;
}
