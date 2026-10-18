/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// x86-64 assembly generation (NASM, Linux System V ABI). Split out of compiler.cc.
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
#include "codegen.h"

using namespace std;

// x86-64 Code Generation (Linux System V ABI, NASM Intel Syntax)

// Maps a mem slot index to its stack offset string for NASM
static std::string slot(int idx)
{
    return "qword [rbp - " + std::to_string((idx + 1) * 8) + "]";
}

static const ColoringResult* g_coloring = nullptr;  // set before codegen

static std::string reg_or_slot(int idx)
{
    if (g_coloring && g_coloring->color.count(idx))
    {
        return REGISTERS[g_coloring->color.at(idx)];
    }
    return "qword [rbp - " + std::to_string((idx + 1) * 8) + "]";
}

void generate_x86(struct InstructionNode* program, const std::string& outputFile,
                  const ColoringResult* coloring)
{
    FILE* out = fopen(outputFile.c_str(), "w");
    g_coloring = coloring;
    if (!out)
    {
        fprintf(stderr, "Error: could not open output file '%s'\n", outputFile.c_str());
        exit(1);
    }

    // ── Pass 1: collect jump targets and function heads ───────────────────────
    map<struct InstructionNode*, int> labelMap;
    map<struct InstructionNode*, int> funcLabelMap;
    set<struct InstructionNode*> funcHeads;
    int labelCounter = 0;

    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == CJMP && pc->cjmp_inst.target != nullptr)
            if (!labelMap.count(pc->cjmp_inst.target))
                labelMap[pc->cjmp_inst.target] = labelCounter++;

        if (pc->type == JMP && pc->jmp_inst.target != nullptr)
            if (!labelMap.count(pc->jmp_inst.target))
                labelMap[pc->jmp_inst.target] = labelCounter++;

        if (pc->type == CALL && pc->call_inst.function_head != nullptr)
        {
            if (!funcLabelMap.count(pc->call_inst.function_head))
                funcLabelMap[pc->call_inst.function_head] = labelCounter++;
            funcHeads.insert(pc->call_inst.function_head);
        }
        pc = pc->next;
    }

    // Functions in label order (= first call in program order). funcLabelMap is keyed by node
    // address, so iterating it would make the output order depend on where malloc put each function.
    vector<pair<int, struct InstructionNode*>> funcOrder;
    for (auto& kv : funcLabelMap) funcOrder.push_back({kv.second, kv.first});
    sort(funcOrder.begin(), funcOrder.end(),
         [](const pair<int, struct InstructionNode*>& a, const pair<int, struct InstructionNode*>& b) {
             return a.first < b.first;
         });

    // ── Stack frame size (for main / spilled slots) ───────────────────────────
    int frameSize = (next_available + 1) * 8;
    if (frameSize % 16 != 0) frameSize += 16 - (frameSize % 16);

    // ── Compute per-function colorings ────────────────────────────────────────
    map<struct InstructionNode*, ColoringResult> funcColorings;
    for (auto& f : funcOrder)
    {
        struct InstructionNode* fhead = f.second;
        LivenessResult flr = compute_liveness(fhead);
        InterferenceGraph fig = build_interference_graph(fhead, flr);
        unordered_set<int> lacSlots = find_live_across_calls(fhead, flr);
        unordered_set<int> constSlots = find_constant_slots(fhead);
        funcColorings[fhead] = color_graph(fig, lacSlots, constSlots);
    }

    for (auto& kv : funcColorings)
    {
        g_funcColorings[kv.first] = kv.second;
    }

    // Insert spill code for functions that have spilled slots
    for (auto& f : funcOrder)
    {
        struct InstructionNode* fhead = f.second;
        if (funcColorings.count(fhead) && !funcColorings[fhead].spilled.empty())
        {
            // Re-run liveness after spill insertion
            fhead = insert_spill_code(fhead, funcColorings[fhead]);
            LivenessResult flr2 = compute_liveness(fhead);
            InterferenceGraph fig2 = build_interference_graph(fhead, flr2);
            unordered_set<int> lac2 = find_live_across_calls(fhead, flr2);
            unordered_set<int> const2 = find_constant_slots(fhead);
            funcColorings[fhead] = color_graph(fig2, lac2, const2);
            // Emit the spill-augmented body under the same label (calls still name the old head)
            funcLabelMap[fhead] = f.first;
            f.second = fhead;
        }
    }

    // ── Data section ──────────────────────────────────────────────────────────
    fprintf(out, "section .data\n");
    fprintf(out, "    fmt_out_int  db \"%%d\", 10, 0\n");
    fprintf(out, "    fmt_out_str  db \"%%s\", 0\n");
    fprintf(out, "    fmt_in_int   db \"%%d\", 0\n");

    for (int i = 0; i < next_str_available; i++)
    {
        fprintf(out, "    strlit_%d db ", i);
        for (unsigned char ch : strMem[i])
            fprintf(out, "%d, ", (int)ch);
        fprintf(out, "10, 0\n");
    }

    // ── Text section ──────────────────────────────────────────────────────────
    fprintf(out, "\nsection .text\n");
    fprintf(out, "    extern printf\n");
    fprintf(out, "    extern scanf\n");
    fprintf(out, "    global main\n\n");

    // ── emitNode lambda ───────────────────────────────────────────────────────
    auto emitNode = [&](struct InstructionNode* pc)
    {
        switch(pc->type)
        {
            case NOOP: break;

            case ASSIGN:
            {
                if (pc->assign_inst.op == OPERATOR_NONE)
                {
                    fprintf(out, "    mov  rax, %s\n", reg_or_slot(pc->assign_inst.operand1_index).c_str());
                    fprintf(out, "    mov  %s, rax\n", reg_or_slot(pc->assign_inst.left_hand_side_index).c_str());
                }
                else
                {
                    fprintf(out, "    mov  rax, %s\n", reg_or_slot(pc->assign_inst.operand1_index).c_str());
                    switch(pc->assign_inst.op)
                    {
                        case OPERATOR_PLUS:
                            fprintf(out, "    add  rax, %s\n", reg_or_slot(pc->assign_inst.operand2_index).c_str());
                            break;
                        case OPERATOR_MINUS:
                            fprintf(out, "    sub  rax, %s\n", reg_or_slot(pc->assign_inst.operand2_index).c_str());
                            break;
                        case OPERATOR_MULT:
                            fprintf(out, "    imul rax, %s\n", reg_or_slot(pc->assign_inst.operand2_index).c_str());
                            break;
                        case OPERATOR_DIV:
                            fprintf(out, "    xor  rdx, rdx\n");
                            fprintf(out, "    cqo\n");
                            fprintf(out, "    mov  rcx, %s\n", reg_or_slot(pc->assign_inst.operand2_index).c_str());
                            fprintf(out, "    idiv rcx\n");
                            break;
                        default: break;
                    }
                    fprintf(out, "    mov  %s, rax\n", reg_or_slot(pc->assign_inst.left_hand_side_index).c_str());
                }
                break;
            }

            case IN:
            {
                fprintf(out, "    lea  rdi, [rel fmt_in_int]\n");
                fprintf(out, "    lea  rsi, %s\n", reg_or_slot(pc->input_inst.var_index).c_str());
                fprintf(out, "    xor  eax, eax\n");
                fprintf(out, "    call scanf\n");
                break;
            }

            case OUT:
            {
                if (!pc->output_inst.is_string)
                {
                    fprintf(out, "    lea  rdi, [rel fmt_out_int]\n");
                    fprintf(out, "    mov  rsi, %s\n", reg_or_slot(pc->output_inst.var_index).c_str());
                    fprintf(out, "    xor  eax, eax\n");
                    fprintf(out, "    call printf\n");
                }
                else
                {
                    int strIdx = pc->output_inst.is_string_var
                                 ? mem[pc->output_inst.var_index]
                                 : pc->output_inst.var_index;
                    fprintf(out, "    lea  rdi, [rel strlit_%d]\n", strIdx);
                    fprintf(out, "    xor  eax, eax\n");
                    fprintf(out, "    call printf\n");
                }
                break;
            }

            case CJMP:
            {
                fprintf(out, "    mov  rax, %s\n", reg_or_slot(pc->cjmp_inst.operand1_index).c_str());
                fprintf(out, "    cmp  rax, %s\n", reg_or_slot(pc->cjmp_inst.operand2_index).c_str());
                int targetLabel = labelMap[pc->cjmp_inst.target];
                switch (pc->cjmp_inst.condition_op)
                {
                    case CONDITION_GREATER:
                        fprintf(out, "    jle  .L%d\n", targetLabel);
                        break;
                    case CONDITION_LESS:
                        fprintf(out, "    jge  .L%d\n", targetLabel);
                        break;
                    case CONDITION_NOTEQUAL:
                        fprintf(out, "    je   .L%d\n", targetLabel);
                        break;
                    case CONDITION_EQUAL:
                        fprintf(out, "    jne  .L%d\n", targetLabel);
                        break;
                }
                break;
            }

            case JMP:
            {
                int targetLabel = labelMap[pc->jmp_inst.target];
                fprintf(out, "    jmp  .L%d\n", targetLabel);
                break;
            }

            case CALL:
            {
                // Get callee's coloring if available
                const ColoringResult* callee_cr = nullptr;
                if (funcColorings.count(pc->call_inst.function_head))
                {
                    callee_cr = &funcColorings[pc->call_inst.function_head];
                }
                for (int i = 0; i < pc->call_inst.num_params; i++)
                {
                    string argLoc   = reg_or_slot(pc->call_inst.arg_val_slots[i]);

                    string paramLoc;
                    int pSlot = pc->call_inst.param_slots[i];
                    if (callee_cr && callee_cr->color.count(pSlot))
                    {
                        paramLoc = REGISTERS[callee_cr->color.at(pSlot)];
                    }
                    else
                    {
                        paramLoc = slot(pSlot);
                    }
                    if (argLoc != paramLoc)
                    {
                        fprintf(out, "    mov  rax, %s\n", argLoc.c_str());
                        fprintf(out, "    mov  %s, rax\n", paramLoc.c_str());
                    }
                }
                int funcLabel = funcLabelMap[pc->call_inst.function_head];
                fprintf(out, "    call nova_f%d\n", funcLabel);
                fprintf(out, "    mov  %s, rax\n", reg_or_slot(pc->call_inst.ret_val_index).c_str());
                break;
            }

            case RET:
            {
                fprintf(out, "    mov  rax, %s\n", reg_or_slot(pc->ret_inst.ret_val_index).c_str());
                // Restore callee-saved registers in reverse order
                if (g_coloring)
                {
                    static const vector<string> CALLEE_SAVED = {
                        "rbx", "r12", "r13", "r14", "r15"
                    };
                    for (int i = (int)CALLEE_SAVED.size() - 1; i >= 0; i--)
                        fprintf(out, "    pop  %s\n", CALLEE_SAVED[i].c_str());
                }
                fprintf(out, "    leave\n");
                fprintf(out, "    ret\n");
                break;
            }

            case ALLOC:
                fprintf(out, "    ; ALLOC base=slot[%d] size=slot[%d]\n",
                    pc->alloc_inst.base_slot, pc->alloc_inst.size_slot);
                break;

            case ARRAY_READ:
            {
                if (pc->array_inst.dynamic_base)
                {
                    fprintf(out, "    mov  rax, %s\n", reg_or_slot(pc->array_inst.base_index).c_str());
                    fprintf(out, "    mov  rcx, %s\n", reg_or_slot(pc->array_inst.index_slot).c_str());
                    fprintf(out, "    add  rax, rcx\n");
                }
                else
                {
                    fprintf(out, "    mov  rcx, %s\n", reg_or_slot(pc->array_inst.index_slot).c_str());
                    fprintf(out, "    mov  rax, %d\n", pc->array_inst.base_index);
                    fprintf(out, "    add  rax, rcx\n");
                }
                fprintf(out, "    inc  rax\n");
                fprintf(out, "    imul rax, 8\n");
                fprintf(out, "    neg  rax\n");
                fprintf(out, "    mov  rdx, [rbp + rax]\n");
                fprintf(out, "    mov  %s, rdx\n", reg_or_slot(pc->array_inst.target_index).c_str());
                break;
            }

            case ARRAY_WRITE:
            {
                fprintf(out, "    mov  rdx, %s\n", reg_or_slot(pc->array_inst.target_index).c_str());
                if (pc->array_inst.dynamic_base)
                {
                    fprintf(out, "    mov  rax, %s\n", reg_or_slot(pc->array_inst.base_index).c_str());
                    fprintf(out, "    mov  rcx, %s\n", reg_or_slot(pc->array_inst.index_slot).c_str());
                    fprintf(out, "    add  rax, rcx\n");
                }
                else
                {
                    fprintf(out, "    mov  rcx, %s\n", reg_or_slot(pc->array_inst.index_slot).c_str());
                    fprintf(out, "    mov  rax, %d\n", pc->array_inst.base_index);
                    fprintf(out, "    add  rax, rcx\n");
                }
                fprintf(out, "    inc  rax\n");
                fprintf(out, "    imul rax, 8\n");
                fprintf(out, "    neg  rax\n");
                fprintf(out, "    mov  [rbp + rax], rdx\n");
                break;
            }

            case SPILL_LOAD:
            {
                // Load from stack into temp register
                fprintf(out, "    mov  rax, %s\n",
                    slot(pc->spill_inst.spill_slot).c_str());
                fprintf(out, "    mov  %s, rax\n",
                    reg_or_slot(pc->spill_inst.temp_slot).c_str());
                break;
            }
            case SPILL_STORE:
            {
                // Store from temp register back to stack
                fprintf(out, "    mov  rax, %s\n",
                    reg_or_slot(pc->spill_inst.temp_slot).c_str());
                fprintf(out, "    mov %s, rax\n",
                    slot(pc->spill_inst.spill_slot).c_str());
                break;
            }

            default:
                fprintf(out, "    ; unhandled IR type %d\n", pc->type);
                break;
        }
    };

    // ── main entry ────────────────────────────────────────────────────────────
    fprintf(out, "main:\n");
    fprintf(out, "    push rbp\n");
    fprintf(out, "    mov  rbp, rsp\n");
    fprintf(out, "    sub  rsp, %d\n\n", frameSize);

    fprintf(out, "    ; initialize constant slots\n");
    for (int i = 0; i < next_available; i++)
        if (mem[i] != 0)
            fprintf(out, "    mov  %s, %d\n", reg_or_slot(i).c_str(), mem[i]);
    fprintf(out, "\n");

    // ── Emit main body ────────────────────────────────────────────────────────
    set<int> emittedLabels;
    pc = program;
    while (pc != nullptr)
    {
        if (funcHeads.count(pc))
        {
            while (pc != nullptr && pc->type != RET) pc = pc->next;
            if (pc != nullptr) pc = pc->next;
            continue;
        }
        if (labelMap.count(pc))
        {
            int lbl = labelMap[pc];
            if (!emittedLabels.count(lbl))
            {
                fprintf(out, ".L%d:\n", lbl);
                emittedLabels.insert(lbl);
            }
        }
        emitNode(pc);
        pc = pc->next;
    }

    fprintf(out, "\n    ; exit\n");
    fprintf(out, "    xor  eax, eax\n");
    fprintf(out, "    leave\n");
    fprintf(out, "    ret\n");

    // ── Emit each function ────────────────────────────────────────────────────
    static const vector<string> CALLEE_SAVED = {
        "rbx", "r12", "r13", "r14", "r15"
    };

    for (auto& f : funcOrder)
    {
        struct InstructionNode* head = f.second;
        int funcLabel = f.first;

        // Switch to this function's coloring
        g_coloring = funcColorings.count(head) ? &funcColorings[head] : coloring;

        fprintf(out, "\n; ── function nova_f%d ─────────────────────────────────\n", funcLabel);
        fprintf(out, "nova_f%d:\n", funcLabel);
        fprintf(out, "    push rbp\n");
        fprintf(out, "    mov  rbp, rsp\n");

        // Save callee-saved registers ONCE — only if this function has coloring
        if (funcColorings.count(head))
        {
            for (auto& reg : CALLEE_SAVED)
                fprintf(out, "    push %s\n", reg.c_str());
        }

        // Compute frame size — only spilled slots need stack space
        int funcFrameSize = 0;
        if (funcColorings.count(head))
        {
            for (int s : funcColorings[head].spilled)
                funcFrameSize = max(funcFrameSize, (s + 1) * 8);
        }
        else
        {
            funcFrameSize = frameSize;
        }
        if (funcFrameSize % 16 != 0) funcFrameSize += 16 - (funcFrameSize % 16);
        if (funcFrameSize > 0)
            fprintf(out, "    sub  rsp, %d\n", funcFrameSize);

        // Initialize constant slots into registers
        unordered_set<int> constSlots = find_constant_slots(head);

        fprintf(out, "    ; initialize constants\n");
        for (int s : constSlots)
            if (funcColorings.count(head) && funcColorings[head].color.count(s))
                fprintf(out, "    mov  %s, %d\n", reg_or_slot(s).c_str(), mem[s]);
        fprintf(out, "\n");

        // Emit function body
        bool afterRet = false;
        struct InstructionNode* fn = head;
        while (fn != nullptr)
        {
            if (fn != head && funcHeads.count(fn)) break;

            if (labelMap.count(fn))
            {
                int lbl = labelMap[fn];
                if (!emittedLabels.count(lbl))
                {
                    fprintf(out, ".L%d:\n", lbl);
                    emittedLabels.insert(lbl);
                }
                afterRet = false;
            }

            if (!afterRet) emitNode(fn);
            if (fn->type == RET) afterRet = true; 
            fn = fn->next;
        }
        fprintf(out, "\n");
    }

    // Restore main coloring
    g_coloring = coloring;

    fclose(out);
    printf("assembly written to: %s\n", outputFile.c_str());
}
