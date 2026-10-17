/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// In-process x86-64 JIT: executable memory, machine-code emitters, compilation. Split out of compiler.cc.
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

#ifdef _WIN32
// Declare only what we need from Windows API - avoids windows.h conflicts
extern "C" {
    void* __stdcall VirtualAlloc(void* lpAddress, unsigned long long dwSize,
                                 unsigned long flAllocationType,
                                 unsigned long flProtect);
    int __stdcall VirtualFree(void* lpAddress, unsigned long long dwSize,
                              unsigned long dwFreeType);
}
static const unsigned long MEM_COMMIT_RESERVE = 0x3000;
static const unsigned long PAGE_EXEC_RW = 0x40;
static const unsigned long MEM_RELEASE_FLAG = 0x8000;

static void* jit_alloc_exec(size_t size)
{
    void* mem = VirtualAlloc(nullptr, (unsigned long long)size,
                             MEM_COMMIT_RESERVE, PAGE_EXEC_RW);
    return mem;
}
static void jit_free_exec(void* mem, size_t)
{
    VirtualFree(mem, 0, MEM_RELEASE_FLAG);
}
#else
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifdef __has_include
#  if __has_include(<libssh2.h>)
#    include <libssh2.h>
#    include <libssh2_sftp.h>
#    define NOVA_HAVE_LIBSSH2 1
#  endif
#endif
static void* jit_alloc_exec(size_t size)
{
    return mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static void jit_free_exec(void* mem, size_t size)
{
    munmap(mem, size);
}
#endif

// JIT Code Generation

// x86-64 Instruction encoding helpers
struct JitBuffer
{
    vector<uint8_t> code;

    void emit(uint8_t byte) { code.push_back(byte); }

    void emit_bytes(initializer_list<uint8_t> bytes)
    {
        for (auto b : bytes) code.push_back(b);
    }

    void emit_u32(uint32_t val)
    {
        code.push_back(val & 0xFF);
        code.push_back((val >> 8) & 0xFF);
        code.push_back((val >> 16) & 0xFF);
        code.push_back((val >> 24) & 0xFF);
    }

    void emit_u64(uint64_t val)
    {
        for (int i = 0;i < 8;i++)
        {
            code.push_back((val >> (i * 8)) & 0xFF);
        }
    }

    // Patch a 32-bit value at a given offset
    void patch_u32(size_t offset, uint32_t val)
    {
        code[offset] = val & 0xFF;
        code[offset + 1] = (val >> 8) & 0xFF;
        code[offset + 2] = (val >> 16) & 0xFF;
        code[offset + 3] = (val >> 24) & 0xFF;
    }

    size_t size() { return code.size(); }
    size_t pos() { return code.size(); }
};

// Register encoding for x86-64 
// We use a subset: rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6, rdi=7
// r8=8, r9=9, r10=10, r11=11, r12=12, r13=13, r14=14, r15=15
static int reg_encode(const string& name)
{
    if (name == "rax") return 0;
    if (name == "rcx") return 1;
    if (name == "rdx") return 2;
    if (name == "rbx") return 3;
    if (name == "rsp") return 4;
    if (name == "rbp") return 5;
    if (name == "rsi") return 6;
    if (name == "rdi") return 7;
    if (name == "r8")  return 8;
    if (name == "r9")  return 9;
    if (name == "r10") return 10;
    if (name == "r11") return 11;
    if (name == "r12") return 12;
    if (name == "r13") return 13;
    if (name == "r14") return 14;
    if (name == "r15") return 15;
    return -1;
}

// Emit: mov reg, reg (64-bit)
static void emit_mov_reg_reg(JitBuffer& buf, int dst, int src)
{
    // REX.W + MOV r/m64, r64
    uint8_t rex = 0x48;
    if (dst >= 8) rex |= 0x01;  // REX.B
    if (src >= 8) rex |= 0x04;  // REX.R
    buf.emit(rex);
    buf.emit(0x89);  // MOV r/m64, r64
    buf.emit(0xc0 | ((src & 7) << 3) | (dst & 7));  // ModRM
}

// Emit: mov reg, imm64
static void emit_mov_reg_imm64(JitBuffer& buf, int reg, int64_t imm)
{
    uint8_t rex = 0x48;
    if (reg >= 8) rex |= 0x01;  // REX.B;
    buf.emit(rex);
    buf.emit(0xB8 | (reg & 7));  // MOV r64, imm64
    buf.emit_u64((uint64_t)imm);
}

// Emit: push reg
static void emit_push(JitBuffer& buf, int reg)
{
    if (reg >= 8)
    {
        buf.emit(0x41);  // REX.B
        buf.emit(0x50 | (reg & 7));
    }
    else
    {
        buf.emit(0x50 | reg);
    }
}

// Emit: pop reg
static void emit_pop(JitBuffer& buf, int reg)
{
    if (reg >= 8)
    {
        buf.emit(0x41);  // REX.B
        buf.emit(0x58 | (reg & 7));
    }
    else
    {
        buf.emit(0x58 | reg);
    }
}

// Emit: add reg, reg
static void emit_add_reg_reg(JitBuffer& buf, int dst, int src)
{
    uint8_t rex = 0x48;
    if (dst >= 8) rex |= 0x01;
    if (src >= 8) rex |= 0x04;
    buf.emit(rex);
    buf.emit(0x01);  // ADD r/m64, r64
    buf.emit(0xC0 | ((src & 7) << 3) | (dst & 7));
}

// Emit: sub reg, reg
static void  emit_sub_reg_reg(JitBuffer& buf, int dst, int src)
{
    uint8_t rex = 0x48;
    if (dst >= 8) rex |= 0x01;
    if (src >= 8) rex |= 0x04;
    buf.emit(rex);
    buf.emit(0x29);  // SUB r/m64, r64
    buf.emit(0xC0 | ((src & 7) << 3) | (dst & 7));
}

// Emit: cmp reg, reg
static void emit_cmp_reg_reg(JitBuffer& buf, int lhs, int rhs)
{
    uint8_t rex = 0x48;
    if (lhs >= 8) rex |= 0x01;
    if (rhs >= 8) rex |= 0x04;
    buf.emit(rex);
    buf.emit(0x39);  // CMP r/m64, r64
    buf.emit(0xC0 | ((rhs & 7) << 3) | (lhs & 7));
}

// Emit: jge rel32 (jump if >=)
static size_t emit_jge_placeholder(JitBuffer& buf)
{
    buf.emit(0x0F);
    buf.emit(0x8D);  // JGE rel32
    size_t patch_pos = buf.pos();
    buf.emit_u32(0); // placeholder
    return patch_pos;
}

// Emit: jmp rel32
static size_t emit_jmp_placeholder(JitBuffer& buf)
{
    buf.emit(0xE9);  // JMP rel32
    size_t patch_pos = buf.pos();
    buf.emit_u32(0);  // placeholder
    return patch_pos;
}

// Emit: call rel32
static size_t emit_call_placeholder(JitBuffer& buf)
{
    buf.emit(0xE8);  // CALL rel32
    size_t patch_pos = buf.pos();
    buf.emit_u32(0);  // placeholder
    return patch_pos;
}

// Emit: ret
static void emit_ret(JitBuffer& buf)
{
    buf.emit(0xC3);
}

// Main JIT compilation function
void* jit_compile_function(struct InstructionNode* head)
{
    // Abort for recursive functions — JIT doesn't yet handle cross-call register
    // preservation correctly for self-recursive functions
    for (struct InstructionNode* pc = head; pc != nullptr; pc = pc->next)
        if (pc->type == CALL && pc->call_inst.function_head == head)
            return nullptr;

    // // Run register allocation for this function
    // LivenessResult lr = compute_liveness(head);
    // fprintf(stderr, "JIT: liveness done\n"); fflush(stderr);

    // InterferenceGraph ig = build_interference_graph(head, lr);
    // fprintf(stderr, "JIT: interference done\n"); fflush(stderr);

    // unordered_set<int> lacSlots   = find_live_across_calls(head, lr);
    // unordered_set<int> constSlots = find_constant_slots(head);
    // ColoringResult cr = color_graph(ig, lacSlots, constSlots);
    // fprintf(stderr, "JIT: coloring done\n"); fflush(stderr);

    // Use cached coloring from generate_x86 if available, otherwise recompute
    ColoringResult cr;
    unordered_set<int> constSlots = find_constant_slots(head);
    unordered_set<int> lacSlots;
    if (g_funcColorings.count(head))
    {
        cr = g_funcColorings[head];
    }
    else
    {
        LivenessResult lr = compute_liveness(head);
        InterferenceGraph ig = build_interference_graph(head, lr);
        lacSlots = find_live_across_calls(head, lr);
        cr = color_graph(ig, lacSlots, constSlots);
    }

    // Helper: get register index for a slot (-1 if spilled)
    auto slotReg = [&](int slot) -> int
    {
        if (cr.color.count(slot))
            return reg_encode(REGISTERS[cr.color.at(slot)]);
        return -1;
    };

    JitBuffer buf;

    // ── Prologue ─────────────────────────────────────────────────────────────
    emit_push(buf, 5);            // push rbp
    emit_mov_reg_reg(buf, 5, 4);  // mov rbp, rsp

    vector<int> calleeSaved = {3, 12, 13, 14, 15};  // rbx, r12-r15
    for (int r : calleeSaved) emit_push(buf, r);

    #ifdef _WIN32
    // sub rsp, 32  (Windows x64 shadow space)
    buf.emit(0x48); buf.emit(0x83); buf.emit(0xEC); buf.emit(0x20);
    #endif

    // Move first argument from ABI register into param register (slot 0 = n)
    #ifdef _WIN32
    int abiArgReg = 1;  // rcx (Microsoft x64 ABI)
    #else
    int abiArgReg = 7;  // rdi (System V ABI)
    #endif
    int paramReg = slotReg(0);
    if (paramReg >= 0 && paramReg != abiArgReg)
        emit_mov_reg_reg(buf, paramReg, abiArgReg);

    // Initialize constant slots into registers
    for (int s : constSlots)
    {
        int r = slotReg(s);
        if (r >= 0 && mem[s] != 0)
            emit_mov_reg_imm64(buf, r, mem[s]);
    }

    // ── IR body emission ─────────────────────────────────────────────────────
    map<struct InstructionNode*, size_t> nodePos;
    vector<pair<size_t, struct InstructionNode*>> patchList;

    struct InstructionNode* pc = head;
    while (pc != nullptr)
    {
        nodePos[pc] = buf.pos();

        switch (pc->type)
        {
            case ASSIGN:
            {
                int dst = slotReg(pc->assign_inst.left_hand_side_index);
                int op1 = slotReg(pc->assign_inst.operand1_index);
                if (dst < 0 || op1 < 0) return nullptr;  // spilled — abort, fall back to interpreter

                if (pc->assign_inst.op == OPERATOR_NONE)
                {
                    if (dst != op1)
                        emit_mov_reg_reg(buf, dst, op1);
                }
                else
                {
                    int op2 = slotReg(pc->assign_inst.operand2_index);
                    if (op2 < 0) return nullptr;  // spilled — abort

                    emit_mov_reg_reg(buf, 0, op1);  // rax = op1
                    switch (pc->assign_inst.op)
                    {
                        case OPERATOR_PLUS:
                            emit_add_reg_reg(buf, 0, op2);  // rax += op2
                            break;
                        case OPERATOR_MINUS:
                            emit_sub_reg_reg(buf, 0, op2);  // rax -= op2
                            break;
                        default:
                            return nullptr;  // unsupported operator — abort
                    }
                    emit_mov_reg_reg(buf, dst, 0);  // dst = rax
                }
                break;
            }

            case CJMP:
            {
                int op1 = slotReg(pc->cjmp_inst.operand1_index);
                int op2 = slotReg(pc->cjmp_inst.operand2_index);
                if (op1 < 0 || op2 < 0) return nullptr;  // spilled — abort

                emit_cmp_reg_reg(buf, op1, op2);

                if (pc->cjmp_inst.condition_op == CONDITION_LESS)
                {
                    size_t patch = emit_jge_placeholder(buf);
                    patchList.push_back({patch, pc->cjmp_inst.target});
                }
                else 
                {
                    // Only "<" has a jump emitter here. Other comparisons used to emit no jump 
                    // at all (the branch was silently always taken); give up and let the interpreter
                    // run this function.
                    return nullptr;
                }
                break;
            }

            case JMP:
            {
                size_t patch = emit_jmp_placeholder(buf);
                patchList.push_back({patch, pc->jmp_inst.target});
                break;
            }

            case CALL:
            {
                // Copy arg into ABI argument register
                for (int i = 0; i < pc->call_inst.num_params; i++)
                {
                    int argReg = slotReg(pc->call_inst.arg_val_slots[i]);
                    if (argReg < 0) return nullptr;  // spilled arg — abort
                    #ifdef _WIN32
                    int callAbiReg = 1;  // rcx
                    #else
                    int callAbiReg = 7;  // rdi
                    #endif
                    if (argReg != callAbiReg)
                        emit_mov_reg_reg(buf, callAbiReg, argReg);
                }

                #ifdef _WIN32
                // sub rsp, 32  (shadow space BEFORE call)
                buf.emit(0x48); buf.emit(0x83); buf.emit(0xEC); buf.emit(0x20);
                #endif
                // Emit call instruction
                size_t patch = emit_call_placeholder(buf);
                patchList.push_back({patch, pc->call_inst.function_head});

                #ifdef _WIN32
                // add rsp, 32  (restore shadow space AFTER call)
                buf.emit(0x48); buf.emit(0x83); buf.emit(0xC4); buf.emit(0x20);
                #endif
                // Store return value
                int retReg = slotReg(pc->call_inst.ret_val_index);
                if (retReg >= 0 && retReg != 0)
                    emit_mov_reg_reg(buf, retReg, 0);  // retReg = rax
                break;
            }

            case RET:
            {
                int retReg = slotReg(pc->ret_inst.ret_val_index);
                if (retReg < 0) return nullptr;  // spilled return value — abort
                if (retReg != 0)
                    emit_mov_reg_reg(buf, 0, retReg);  // rax = retReg

                #ifdef _WIN32
                // add rsp, 32  (remove shadow space)
                buf.emit(0x48); buf.emit(0x83); buf.emit(0xC4); buf.emit(0x20);
                #endif
                // Restore callee-saved in reverse
                for (int i = (int)calleeSaved.size() - 1; i >= 0; i--)
                    emit_pop(buf, calleeSaved[i]);

                emit_pop(buf, 5);  // pop rbp
                emit_ret(buf);
                break;
            }

            case NOOP:
            default:
                break;
        }

        if (pc->type == RET)
        {
            pc = pc->next;
            continue;
        }
        pc = pc->next;
    }
    // ── Allocate executable memory ────────────────────────────────────────────
    size_t codeSize = buf.size();
    if (codeSize == 0) return nullptr;

    uint8_t* execMem = (uint8_t*)jit_alloc_exec(codeSize);
    if (!execMem) return nullptr;

    memcpy(execMem, buf.code.data(), codeSize);
    execMem[0] = execMem[0];  // test write

    // ── Patch jumps and calls ─────────────────────────────────────────────────

    for (size_t pi = 0; pi < patchList.size();pi++)
    {
        size_t patchPos = patchList[pi].first;
        struct InstructionNode* targetNode = patchList[pi].second;

        size_t targetOffset;

        // Check if this is a branch (CJMP/JMP) or a call
        // Branches target IR nodes within this function
        // Calls are always recursive self-calls → target offset 0
        bool isCall = (patchPos > 0 && execMem[patchPos - 1] == 0xE8);

        if (isCall)
        {
            targetOffset = 0; // recursive self-call -> start of function
        }
        else if (nodePos.count(targetNode))
        {
            targetOffset = nodePos[targetNode];
        }
        else
        {
            targetOffset = 0;
        }

        int32_t rel = (int32_t)((int64_t)targetOffset - (int64_t)(patchPos + 4));
        execMem[patchPos + 0] = rel & 0xFF;
        execMem[patchPos + 1] = (rel >>  8) & 0xFF;
        execMem[patchPos + 2] = (rel >> 16) & 0xFF;
        execMem[patchPos + 3] = (rel >> 24) & 0xFF;
    }

    return execMem;
}
