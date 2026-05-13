/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
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
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include "lexer.h"
#include "compiler.h"
#include "bytecode.h"
#include "stdlib/novatorch.tqdm.h"
#include "autograd.h"

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

using namespace std;

#define DEBUG 1

extern LexicalAnalyzer lexer;
extern vector<string> errorList;
struct InstructionNode* parse_repl_input(const string& input);
extern map<string, InstructionNode*> functionTable;

bool suppress_output = false;

// Debug mode
bool debug_mode = false;
bool debug_step = false;  // true = pause on every instruction, false = continue until breakpoint
set<int> debug_breakpoints;  // set of line numbers with break points

// forward declaration
extern map<string, int> symbolTable;
extern map<string, int> floatSymbolTable;
extern map<string, int> doubleSymbolTable;

// Structs and data structures for Graph Coloring Register Allocation

// Liveness Analysis
struct LivenessResult {
    std::unordered_map<InstructionNode*, std::unordered_set<int>> live_in;
    std::unordered_map<InstructionNode*, std::unordered_set<int>> live_out;
};

// Control Flow Interference Graph
struct InterferenceGraph {
    unordered_map<int, unordered_set<int>> adj; // slot -> set of interfacing slots

    void add_edge(int u, int v)
    {
        if (u == v) return; // no self-loops
        adj[u].insert(v);
        adj[v].insert(u);
    }

    void add_node(int u)
    {
        if (!adj.count(u)) adj[u] = {};
    }

    int degree(int u)
    {
        return adj.count(u) ? (int)adj[u].size() : 0;
    }
};

// Graph Coloring (Chaintin-Briggs)
// x86-64 caller-saved registers available for allocation
// we exclude rax (used for return values and scratch), rsp, rbp
static const vector<string> REGISTERS = {
    "rbx", "r12", "r13", "r14", "r15",  // callee-saved - safe across calls
    "rcx", "rdx", "rsi", "rdi",  // caller-saved - clobbered by calls
    "r8", "r9", "r10", "r11"  // caller-saved
};
static const int K = (int)REGISTERS.size();  // number of available registers

struct ColoringResult
{
    unordered_map<int, int> color;  // slot - register index (into REGISTERS)
    unordered_set<int> spilled;  // slots that couldn't be colored
};

unordered_map<InstructionNode*, ColoringResult> g_funcColorings;

// JIT
static const int JIT_THRESHOLD = 1;
unordered_map<InstructionNode*, int> jit_call_counts;
unordered_map<InstructionNode*, void*> jit_compiled;
unordered_set<InstructionNode*> jit_deoptimized;

// Memoization for pure recursive functions
unordered_set<InstructionNode*> pure_functions;
unordered_set<InstructionNode*> recursive_functions;
map<InstructionNode*, map<vector<int>, vector<int>>> memo_cache;

// struct for Call stack
struct CallFrame {
    struct InstructionNode* returnAddress;
    int dest_index;
    int slot_base;
    vector<int> savedSlots;
    vector<int> allRetSlots;
    bool is_memoizable = false;
    InstructionNode* memo_func = nullptr;
    vector<int> memo_key;
};

// Add near debug globals
map<string, pair<int, VarType>> debug_symbol_snapshot;  // name -> {slot, type}
int import_line_offset = 0; // lines added by imports - used to skip imported code

// Map Definitions for Class 
map<string, ClassDef> classTable;
map<string, string> varClassType;
map<string, map<string, int>> classFieldSlots;
map<string, map<string, VarType>> classFieldTypes;
map<string, map<string, InstructionNode*>> classMethodTable;
map<string, map<string, vector<string>>> classMethodParams;
map<string, map<string, vector<VarType>>> classMethodParamTypes;
map<string, map<string, VarType>> classMethodReturnType;
int currentSelfBase = -1;
string currentSelfName = "";
map<string, map<string, map<string, int>>> classMethodSelfSlots;
map<string, map<string, map<string, VarType>>> classMethodSelfTypes;

// Global memory vectors

vector<int> mem;
int next_available = 0;

vector<std::string> strMem;
int next_str_available = 0;

string hf_api_token;

static string sol_key_path = "";
static string sol_host     = "localhost";
static int    sol_port     = 2223;
static string sol_username = "pmathu14";
static string sol_work_dir = "/scratch/pmathu14/novacomp_jobs";

vector<int> freeList;

vector<float> fmem;
int next_float_available = 0;

vector<double> dmem;
int next_double_available = 0;

// Slot allocator
int alloc_slot()
{
    if (!freeList.empty())
    {
        int idx = freeList.back();
        freeList.pop_back();
        mem[idx] = 0;
        return idx; 
    }
    mem.push_back(0);
    return next_available++;
}

void free_slot(int idx)
{
    freeList.push_back(idx);
}

int alloc_float_slot()
{
    fmem.push_back(0.0f);
    return next_float_available++;
}

int alloc_double_slot()
{
    dmem.push_back(0.0);
    return next_double_available++;
}

// Tensor heap
std::vector<Tensor> tensor_heap;

int alloc_tensor(int rows, int cols)
{
    Tensor t;
    t.rows = rows;
    t.cols = cols;
    t.data.assign(rows * cols, 0.0);
    tensor_heap.push_back(std::move(t));
    return (int)tensor_heap.size() - 1;
}

static std::mt19937 rng(42);

struct ProgressBar {
    int total;
    int width;
};

static vector<ProgressBar> progress_heap;

static int ten_zeros(int r, int c) { return alloc_tensor(r, c); }

static int ten_ones(int r, int c)
{
    int h = alloc_tensor(r, c);
    for (auto& v : tensor_heap[h].data) v = 1.0;
    return h;
}

static int ten_randn(int r, int c)
{
    std::normal_distribution<double> dist(0.0, 1.0);
    int h = alloc_tensor(r, c);
    for (auto& v : tensor_heap[h].data) v = dist(rng);
    return h;
}

static int ten_rand_int(int lo, int hi)
{
    std::uniform_int_distribution<int> dist(lo, hi);
    int h = alloc_tensor(1, 1);
    tensor_heap[h].at(0, 0) = (double)dist(rng);
    return h;
}

static int ten_xavier(int r, int c)
{
    double limit = std::sqrt(6.0 / (r + c));
    std::uniform_real_distribution<double> dist(-limit, limit);
    int h = alloc_tensor(r, c);
    for (auto& v : tensor_heap[h].data) v = dist(rng);
    return h;
}

static int ten_matmul(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int bCols = tensor_heap[b].cols;
    int h = alloc_tensor(aRows, bCols);
    for (int i = 0; i < aRows; i++)
        for (int k = 0; k < aCols; k++)
            for (int j = 0; j < bCols; j++)
                tensor_heap[h].at(i, j) += tensor_heap[a].at(i, k) * tensor_heap[b].at(k, j);
    return h;
}

static int ten_add(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int bRows = tensor_heap[b].rows, bCols = tensor_heap[b].cols;
    int h;
    if (aRows == bRows && aCols == bCols)
    {
        h = alloc_tensor(aRows, aCols);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] + tensor_heap[b].data[i];
    }
    else if (bRows == 1 && aCols == bCols)
    {
        h = alloc_tensor(aRows, aCols);
        for (int i = 0; i < aRows; i++)
            for (int j = 0; j < aCols; j++)
                tensor_heap[h].at(i, j) = tensor_heap[a].at(i, j) + tensor_heap[b].at(0, j);
    }
    else if (aRows == 1 && aCols == bCols)
    {
        h = alloc_tensor(bRows, bCols);
        for (int i = 0; i < bRows; i++)
            for (int j = 0; j < bCols; j++)
                tensor_heap[h].at(i, j) = tensor_heap[a].at(0, j) + tensor_heap[b].at(i, j);
    }
    else
    {
        h = alloc_tensor(aRows, aCols);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] + tensor_heap[b].data[i];
    }
    return h;
}

static int ten_sub(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int bRows = tensor_heap[b].rows, bCols = tensor_heap[b].cols;
    int h;
    if (aRows == bRows && aCols == bCols)
    {
        h = alloc_tensor(aRows, aCols);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] - tensor_heap[b].data[i];
    }
    else if (bRows == 1 && aCols == bCols)
    {
        h = alloc_tensor(aRows, aCols);
        for (int i = 0; i < aRows; i++)
            for (int j = 0; j < aCols; j++)
                tensor_heap[h].at(i, j) = tensor_heap[a].at(i, j) - tensor_heap[b].at(0, j);
    }
    else
    {
        h = alloc_tensor(aRows, aCols);
        for (int i = 0; i < (int)tensor_heap[a].data.size(); i++)
            tensor_heap[h].data[i] = tensor_heap[a].data[i] - tensor_heap[b].data[i];
    }
    return h;
}

static int ten_sum_axis0(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int h = alloc_tensor(1, aCols);
    for (int j = 0; j < aCols; j++)
    {
        double s = 0.0;
        for (int i = 0; i < aRows; i++) s += tensor_heap[a].at(i, j);
        tensor_heap[h].at(0, j) = s;
    }
    return h;
}

static int ten_mul(int a, int b)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = tensor_heap[a].data[i] * tensor_heap[b].data[i];
    return h;
}

static int ten_scale(int a, double s)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = tensor_heap[a].data[i] * s;
    return h;
}

static int ten_transpose(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int h = alloc_tensor(aCols, aRows);
    for (int i = 0; i < aRows; i++)
        for (int j = 0; j < aCols; j++)
            tensor_heap[h].at(j, i) = tensor_heap[a].at(i, j);
    return h;
}

static int ten_clip(int a, double lo, double hi)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = std::max(lo, std::min(hi, tensor_heap[a].data[i]));
    return h;
}

static int ten_relu(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = std::max(0.0, tensor_heap[a].data[i]);
    return h;
}

static int ten_sigmoid(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = 1.0 / (1.0 + std::exp(-tensor_heap[a].data[i]));
    return h;
}

static int ten_tanh(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = std::tanh(tensor_heap[a].data[i]);
    return h;
}

static int ten_softmax(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < aRows; i++)
    {
        double maxv = tensor_heap[a].at(i, 0);
        for (int j = 1; j < aCols; j++) maxv = std::max(maxv, tensor_heap[a].at(i, j));
        double sum = 0.0;
        for (int j = 0; j < aCols; j++)
        {
            tensor_heap[h].at(i, j) = std::exp(tensor_heap[a].at(i, j) - maxv);
            sum += tensor_heap[h].at(i, j);
        }
        for (int j = 0; j < aCols; j++) tensor_heap[h].at(i, j) /= sum;
    }
    return h;
}

static int ten_relu_grad(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = tensor_heap[a].data[i] > 0.0 ? 1.0 : 0.0;
    return h;
}

static int ten_sigmoid_grad(int a)
{
    int aRows = tensor_heap[a].rows, aCols = tensor_heap[a].cols;
    int sz = (int)tensor_heap[a].data.size();
    int h = alloc_tensor(aRows, aCols);
    for (int i = 0; i < sz; i++)
    {
        double s = 1.0 / (1.0 + std::exp(-tensor_heap[a].data[i]));
        tensor_heap[h].data[i] = s * (1.0 - s);
    }
    return h;
}

static double ten_mse_loss(int pred, int target)
{
    Tensor& P = tensor_heap[pred];
    Tensor& T = tensor_heap[target];
    double sum = 0.0;
    for (int i = 0; i < (int)P.data.size(); i++)
    {
        double d = P.data[i] - T.data[i];
        sum += d * d;
    }
    return sum / P.data.size();
}

static int ten_mse_grad(int pred, int target)
{
    int pRows = tensor_heap[pred].rows, pCols = tensor_heap[pred].cols;
    int sz = (int)tensor_heap[pred].data.size();
    int h = alloc_tensor(pRows, pCols);
    double scale = 2.0 / sz;
    for (int i = 0; i < sz; i++)
        tensor_heap[h].data[i] = scale * (tensor_heap[pred].data[i] - tensor_heap[target].data[i]);
    return h;
}

static int ten_bce_grad(int pred, int target)
{
    int pRows = tensor_heap[pred].rows;
    int pCols = tensor_heap[pred].cols;
    int size = (int)tensor_heap[pred].data.size();
    int h = alloc_tensor(pRows, pCols);
    double scale = 1.0 / size;
    for (int i = 0;i < size;i++)
    {
        // gradient = (pred - Y) / N (after sigmoid cancellation)
        tensor_heap[h].data[i] = scale * (tensor_heap[pred].data[i] - tensor_heap[target].data[i]);
    }
    return h;
}

static double ten_ce_loss(int logits, int target)
{
    Tensor& L = tensor_heap[logits];
    Tensor& T = tensor_heap[target];
    double loss = 0.0;
    for (int i = 0; i < L.rows; i++)
    {
        double maxv = L.at(i, 0);
        for (int j = 1; j < L.cols; j++) maxv = std::max(maxv, L.at(i, j));
        double sum = 0.0;
        for (int j = 0; j < L.cols; j++) sum += std::exp(L.at(i, j) - maxv);
        double log_sum = std::log(sum) + maxv;
        for (int j = 0; j < L.cols; j++) loss -= T.at(i, j) * (L.at(i, j) - log_sum);
    }
    return loss / L.rows;
}

static int ten_ce_grad(int logits, int target)
{
    int lRows = tensor_heap[logits].rows, lCols = tensor_heap[logits].cols;
    int h = ten_softmax(logits);  // safe: reads logits by index inside, no held ref
    for (int i = 0; i < lRows; i++)
        for (int j = 0; j < lCols; j++)
            tensor_heap[h].at(i, j) = (tensor_heap[h].at(i, j) - tensor_heap[target].at(i, j)) / lRows;
    return h;
}

//Extract a single row as 1xN tensor
static int ten_get_row(int a, int row)
{
    int cols = tensor_heap[a].cols;
    int h = alloc_tensor(1, cols);
    for (int j = 0;j < cols;j++)
    {
        tensor_heap[h].at(0, j) = tensor_heap[a].at(row, j);
    }
    return h;
}

// Copy src 1xN tensor into row of dst
static void ten_set_row(int dst, int row, int src)
{
    int cols = tensor_heap[dst].cols;
    for (int j = 0;j < cols;j++)
    {
        tensor_heap[dst].at(row, j) = tensor_heap[src].at(0, j);
    }
}

// Copy rows [start, end) into a new tensor
static int ten_slice_rows(int a, int start, int end)
{
    int cols = tensor_heap[a].cols;
    int h = alloc_tensor(end - start, cols);
    for (int i = start;i < end;i++)
    {
        for (int j = 0;j < cols;j++)
        {
            tensor_heap[h].at(i - start, j) = tensor_heap[a].at(i, j);
        }
    }
    return h;
}

static int ten_progress_bar(int total, int width)
{
    return tqdm_init(total, width);
}

static void ten_progress_update(int handle, int epoch, double loss)
{
    tqdm_update(handle, epoch, loss);
}

static void ten_progress_done(int handle)
{
    tqdm_done(handle);
}

static int ten_load_csv(const string& path, int rows, int cols)
{
    ifstream f(path);
    if (!f.is_open())
    {
        fprintf(stderr, "tensor_load_csv: cannot open '%s'\n", path.c_str());
        return -1;
    }
    int h = alloc_tensor(rows, cols);
    string line;
    int row = 0;
    while (getline(f, line) && row < rows)
    {
        istringstream ss(line);
        string token;
        int col = 0;
        while (getline(ss, token, ',') && col < cols)
        {
            try { tensor_heap[h].at(row, col) = stod(token); }
            catch (...) {}
            col++;
        }
        row++;
    }
    return h;
}

// ── Input replay buffer ───────────────────────────────────────────────────────
// Used only during benchmark mode — captures inputs from first run
// and replays them for subsequent timed runs
static vector<int> input_replay_buffer;
static int         input_replay_index = -1;  // -1 = live stdin mode

stack<CallFrame> callStack;

void debug(const char* format, ...)
{
    va_list args;
    if (DEBUG)
    {
        va_start(args, format);
        vfprintf(stdout, format, args);
        va_end(args);
    }
}

void debug_print_state(int line_no)
{
    // Build JSON with current line + all variable values
    printf("__DEBUG__{\"type\":\"paused\",\"line\":%d,\"vars\":{", line_no);

    bool first = true;
    // Int variables
    for (auto& kv : debug_symbol_snapshot)
    {
        int slot = kv.second.first;
        VarType type = kv.second.second;
        if (!first) printf(",");
        if (type == TYPE_FLOAT && slot < (int)fmem.size())
        {
            printf("\"%s\":{\"type\":\"float\",\"value\":%.4f}", kv.first.c_str(), fmem[slot]);
        }
        else if (type == TYPE_DOUBLE && slot < (int)dmem.size())
        {
            printf("\"%s\":{\"type\":\"double\",\"value\":%.4f}", kv.first.c_str(), dmem[slot]);
        }
        else if (slot < (int)mem.size())
        {
            printf("\"%s\":{\"type\":\"int\",\"value\":%d}", kv.first.c_str(), mem[slot]);
        }
        first = false;
    }
    printf("}}\n");
    fflush(stdout);
}

string debug_wait_command()
{
    char buf[256];
    while (fgets(buf, sizeof(buf), stdin))
    {
        string cmd(buf);
        // trim whitespace
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
        {
            cmd.pop_back();
        }
        if (cmd == "step" || cmd == "continue" || cmd == "stop")
        {
            return cmd;
        }

        // Handle break points: "breakpoints:5,12,20"
        if (cmd.size() >= 12 && cmd.substr(0, 12) == "breakpoints:")
        {
            string nums = cmd.substr(12);
            debug_breakpoints.clear();
            if (!nums.empty())
            {
                stringstream ss(nums);
                string token;
                while (getline(ss, token, ','))
                {
                    try { debug_breakpoints.insert(stoi(token) + import_line_offset); }
                    catch(...) {}
                }
            }
            continue;
        }
    }
    return "stop";
}

string read_file(const string& path) 
{
    ifstream f(path);
    if (!f.is_open()) {
        cerr << "Error: cannot open file '" << path << "'\n";
        return "";
    }
    ostringstream ss;
    ss << f.rdbuf();
    string content = ss.str();
    // Strip UTF-8 BOM if present
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF) {
        content = content.substr(3);
    }
    return content;
}

string get_stdlib_path()
{
    const char* nova_home = getenv("NOVA_HOME");
     fprintf(stderr, "NOVA_HOME=%s\n", nova_home ? nova_home : "(not set)");
    if (!nova_home)
    {
        cerr << "Error: NOVA_HOME environment variable is not set.\n";
        cerr << "       Set it to your Nova install directory (e.g. export NOVA_HOME=/usr/local/nova)\n";
        return "";
    }
    string path = string(nova_home);
    // normalize: strip trailing slash if present
    if (!path.empty() && (path.back() == '/' || path.back() == '\\'))
    {
        path.pop_back();
    }
    return path + "/stdlib";
}

string preprocess_import(const string& src, const string& base_dir, set<string>& already_imported)
{
    string result;
    istringstream stream(src);
    string line;

    while (getline(stream, line))
    {
        // trim leading whitespace for matching
        string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != string::npos) trimmed = trimmed.substr(start);

        // mathc: import "file.nova";
        if (trimmed.size() > 8 && trimmed.substr(0, 7) == "import " && trimmed[7] == '"')
        {
            size_t close = trimmed.find('"', 8);
            if (close != string::npos)
            {
                string filename = trimmed.substr(8, close - 8);
                string full_path = base_dir + "/" + filename;

                if (already_imported.count(full_path))
                {
                    // already included, skip silently
                    continue;
                }
                already_imported.insert(full_path);

                string file_src = read_file(full_path);
                if (file_src.empty()) continue;

                // get the directory of the imported file for nested imports
                string imported_dir = full_path;
                size_t last_slash = imported_dir.find_last_of("/\\");
                if (last_slash != string::npos)
                {
                    imported_dir = imported_dir.substr(0, last_slash);
                }
                else
                {
                    imported_dir = ".";
                }

                string inlined = preprocess_import(file_src, imported_dir, already_imported);
                result += inlined + "\n";
                continue;
            }
        }

        // match: import <package> ;
        if (trimmed.size() > 8 && trimmed.substr(0, 7) == "import " && trimmed[7] == '<')
        {
            size_t close = trimmed.find('>', 8);
            if (close != string::npos)
            {
                string pkg_name = trimmed.substr(8, close - 8);
                string stdlib_dir = get_stdlib_path();
                if (stdlib_dir.empty()) continue;

                string full_path = stdlib_dir + "/" + pkg_name + ".nova";

                if (already_imported.count(full_path))
                {
                    continue;
                }
                already_imported.insert(full_path);

                string file_src = read_file(full_path);
                if (file_src.empty()) continue;

                string inlined = preprocess_import(file_src, stdlib_dir, already_imported);
                result += inlined + "\n";
                continue;
            }
        }

        // match: import hf  (HuggingFace built-in — hf.xxx() always available)
        if (trimmed.size() >= 9 && trimmed.substr(0, 9) == "import hf" &&
            (trimmed.size() == 9 || trimmed[9] == ' ' || trimmed[9] == ';'))
        {
            continue;
        }

        // match: import sol  (SOL cluster built-in — sol.xxx() always available)
        if (trimmed.size() >= 10 && trimmed.substr(0, 10) == "import sol" &&
            (trimmed.size() == 10 || trimmed[10] == ' ' || trimmed[10] == ';'))
        {
            continue;
        }

        // not an import line - keep it as it is
        result += line + "\n";
    }

    return result;
}

// Forward declaration for JIT
static void* jit_compile_function(struct InstructionNode* head);

// Scan functionTable to identify pure + recursive functions eligible for memoization.
// Pure: no IN, OUT, TENSOR_CALL, ARRAY_WRITE, or ALLOC nodes in the body.
// Recursive: body contains a CALL back to its own head.
void detect_memoizable_functions()
{
    pure_functions.clear();
    recursive_functions.clear();

    // Walk every node in the function's linked list (no break at RET) so that
    // nodes appearing after a conditional early-return are not missed.
    for (auto& kv : functionTable)
    {
        InstructionNode* head = kv.second;
        bool is_pure = true;
        for (InstructionNode* n = head; n != nullptr; n = n->next)
        {
            if (n->type == IN || n->type == OUT || n->type == TENSOR_CALL ||
                n->type == ARRAY_WRITE || n->type == ALLOC)
            {
                is_pure = false;
                break;
            }
        }
        if (is_pure) pure_functions.insert(head);
    }

    for (auto& kv : functionTable)
    {
        InstructionNode* head = kv.second;
        for (InstructionNode* n = head; n != nullptr; n = n->next)
        {
            if (n->type == CALL && n->call_inst.function_head == head)
            {
                recursive_functions.insert(head);
                break;
            }
        }
    }
}

// ── SOL cluster helpers ───────────────────────────────────────────────────────

static string sol_trim(const string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void sol_write_file(const string& path, const string& content)
{
    ofstream f(path);
    f << content;
}

static string sol_extract_slurm_id(const string& sbatchOut)
{
    // "Submitted batch job 12345" → "12345"
    string s = sol_trim(sbatchOut);
    size_t pos = s.rfind(' ');
    return (pos != string::npos) ? s.substr(pos + 1) : s;
}

#ifdef NOVA_HAVE_LIBSSH2

static string sol_get_key()
{
    if (!sol_key_path.empty()) return sol_key_path;
    const char* env = getenv("SOL_PRIVATE_KEY_PATH");
    return env ? string(env) : "";
}

static int sol_open_socket()
{
    struct sockaddr_in sin;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons((uint16_t)sol_port);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        close(sock); return -1;
    }
    return sock;
}

static LIBSSH2_SESSION* sol_open_session(int sock)
{
    static bool libssh2_ready = false;
    if (!libssh2_ready) { libssh2_init(0); libssh2_ready = true; }

    LIBSSH2_SESSION* s = libssh2_session_init();
    if (!s) return nullptr;
    if (libssh2_session_handshake(s, sock) != 0) {
        libssh2_session_free(s); return nullptr;
    }
    string key = sol_get_key();
    if (key.empty()) {
        fprintf(stderr, "Nova: SOL private key not configured. Set SOL_PRIVATE_KEY_PATH on the server.\n");
        libssh2_session_free(s); return nullptr;
    }
    if (libssh2_userauth_publickey_fromfile(
            s, sol_username.c_str(), nullptr, key.c_str(), nullptr) != 0) {
        fprintf(stderr, "Nova: SOL SSH auth failed — check key path: %s\n", key.c_str());
        libssh2_session_free(s); return nullptr;
    }
    return s;
}

static void sol_close_session(LIBSSH2_SESSION* s, int sock)
{
    libssh2_session_disconnect(s, "Normal shutdown");
    libssh2_session_free(s);
    close(sock);
}

static string sshExecSOL(const string& command)
{
    int sock = sol_open_socket();
    if (sock < 0) {
        fprintf(stderr, "Nova: SOL tunnel not available (localhost:%d). "
                "Start the tunnel with: autossh -M 0 -f -N -R 2222:localhost:22 ubuntu@<ec2-ip>\n",
                sol_port);
        return "";
    }
    LIBSSH2_SESSION* session = sol_open_session(sock);
    if (!session) { close(sock); return ""; }

    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(session);
    if (!ch) { sol_close_session(session, sock); return ""; }

    libssh2_channel_exec(ch, command.c_str());

    string result;
    char buf[4096];
    int n;
    while ((n = libssh2_channel_read(ch, buf, sizeof(buf))) > 0)
        result.append(buf, n);

    libssh2_channel_send_eof(ch);
    libssh2_channel_free(ch);
    sol_close_session(session, sock);
    return result;
}

static void sftpUploadSOL(const string& localPath, const string& remotePath)
{
    int sock = sol_open_socket();
    if (sock < 0) { fprintf(stderr, "Nova: SOL tunnel not available.\n"); return; }
    LIBSSH2_SESSION* session = sol_open_session(sock);
    if (!session) { close(sock); return; }

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session);
    if (!sftp) { sol_close_session(session, sock); return; }

    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, remotePath.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (fh) {
        ifstream in(localPath, ios::binary);
        char buf[16384];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
            libssh2_sftp_write(fh, buf, (size_t)in.gcount());
        libssh2_sftp_close(fh);
    }
    libssh2_sftp_shutdown(sftp);
    sol_close_session(session, sock);
}

static void sftpDownloadSOL(const string& remotePath, const string& localPath)
{
    int sock = sol_open_socket();
    if (sock < 0) { fprintf(stderr, "Nova: SOL tunnel not available.\n"); return; }
    LIBSSH2_SESSION* session = sol_open_session(sock);
    if (!session) { close(sock); return; }

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session);
    if (!sftp) { sol_close_session(session, sock); return; }

    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, remotePath.c_str(),
        LIBSSH2_FXF_READ, 0);
    if (fh) {
        ofstream out(localPath, ios::binary);
        char buf[16384]; int n;
        while ((n = (int)libssh2_sftp_read(fh, buf, sizeof(buf))) > 0)
            out.write(buf, n);
        libssh2_sftp_close(fh);
    }
    libssh2_sftp_shutdown(sftp);
    sol_close_session(session, sock);
}

#else  // no libssh2 or Windows

static string sshExecSOL(const string&)
{
    fprintf(stderr, "Nova: SOL cluster requires libssh2. "
            "Install with: sudo apt install libssh2-1-dev\n");
    return "";
}
static void sftpUploadSOL(const string&, const string&)
{
    fprintf(stderr, "Nova: SOL cluster requires libssh2.\n");
}
static void sftpDownloadSOL(const string&, const string&)
{
    fprintf(stderr, "Nova: SOL cluster requires libssh2.\n");
}

#endif  // NOVA_HAVE_LIBSSH2

static string sol_replace_all(string tmpl, const string& from, const string& to)
{
    size_t pos = 0;
    while ((pos = tmpl.find(from, pos)) != string::npos) {
        tmpl.replace(pos, from.size(), to);
        pos += to.size();
    }
    return tmpl;
}

static string generateTrainScript(const string& model, const string& task,
                                   const string& dsName, const string& dsSplit,
                                   int epochs, float lr, int batchSize,
                                   const string& outputDir, const string& hfTok)
{
    string t = R"NOVA(#!/usr/bin/env python3
# Generated by NovaComp sol.finetune()
import os, json
from datasets import load_dataset
from transformers import (AutoTokenizer, AutoModelForSequenceClassification,
                          TrainingArguments, Trainer)

MODEL_NAME   = "{{MODEL_NAME}}"
TASK         = "{{TASK}}"
DATASET_NAME = "{{DATASET_NAME}}"
DATASET_SPLIT= "{{DATASET_SPLIT}}"
EPOCHS       = {{EPOCHS}}
LR           = {{LR}}
BATCH_SIZE   = {{BATCH_SIZE}}
OUTPUT_DIR   = "{{OUTPUT_DIR}}"
HF_TOKEN     = "{{HF_TOKEN}}"

print(f"Loading model: {MODEL_NAME}")
tokenizer = AutoTokenizer.from_pretrained(MODEL_NAME, token=HF_TOKEN)
model = AutoModelForSequenceClassification.from_pretrained(
    MODEL_NAME, token=HF_TOKEN, num_labels=2)

print(f"Loading dataset: {DATASET_NAME}/{DATASET_SPLIT}")
dataset = load_dataset(DATASET_NAME, split=DATASET_SPLIT, token=HF_TOKEN)

def tokenize(examples):
    return tokenizer(examples["text"], truncation=True, padding=True, max_length=512)

tokenized = dataset.map(tokenize, batched=True)

training_args = TrainingArguments(
    output_dir=OUTPUT_DIR,
    num_train_epochs=EPOCHS,
    per_device_train_batch_size=BATCH_SIZE,
    learning_rate=LR,
    save_strategy="epoch",
    logging_dir=f"{OUTPUT_DIR}/logs",
    report_to="none"
)

trainer = Trainer(model=model, args=training_args, train_dataset=tokenized)
print("Starting training...")
trainer.train()
print("Saving model...")
trainer.save_model(OUTPUT_DIR)
tokenizer.save_pretrained(OUTPUT_DIR)

# Ensure pytorch_model.bin exists — login node may have older transformers
# that cannot load safetensors format.
import os as _os
_st = _os.path.join(OUTPUT_DIR, 'model.safetensors')
_pt = _os.path.join(OUTPUT_DIR, 'pytorch_model.bin')
if _os.path.exists(_st) and not _os.path.exists(_pt):
    try:
        from safetensors.torch import load_file as _lf
        import torch as _torch
        _torch.save(_lf(_st), _pt)
        print("Converted model.safetensors -> pytorch_model.bin for compatibility")
    except Exception as _e:
        print(f"[Warning] safetensors->pytorch conversion failed: {_e}")


# Upload trained model to user's file storage via pre-signed S3 URL
_upload_url = "{{UPLOAD_URL}}"
_s3_key     = "{{S3_KEY}}"
if _upload_url:
    print("Packaging model for user file storage...")
    import subprocess as _sp
    _tar = OUTPUT_DIR.rstrip('/') + '.tar.gz'
    _sp.run(['tar', '-czf', _tar, '-C', _os.path.dirname(OUTPUT_DIR),
             _os.path.basename(OUTPUT_DIR)], check=True)
    _r = _sp.run(
        ['curl', '-s', '-o', '/dev/null', '-w', '%{http_code}',
         '-X', 'PUT', '-H', 'Content-Type: application/gzip',
         '-T', _tar, _upload_url],
        capture_output=True, text=True)
    _code = _r.stdout.strip()
    if _code == '200':
        print(f"NOVA_MODEL_UPLOADED:{_s3_key}")
        print("Model uploaded to your file storage successfully.")
    else:
        print(f"[Warning] Model upload to file storage failed (HTTP {_code})")
    _sp.run(['rm', '-f', _tar])

summary = {"model": MODEL_NAME, "task": TASK, "epochs": EPOCHS, "status": "completed"}
with open(f"{OUTPUT_DIR}/summary.json", "w") as f:
    json.dump(summary, f)
print(f"Training complete! Model saved to {OUTPUT_DIR}")
)NOVA";

    char lrBuf[32];
    snprintf(lrBuf, sizeof(lrBuf), "%.8f", (double)lr);

    const char* uploadUrlEnv = getenv("NOVA_MODEL_UPLOAD_URL");
    const char* s3KeyEnv     = getenv("NOVA_MODEL_S3_KEY");

    t = sol_replace_all(t, "{{MODEL_NAME}}",    model);
    t = sol_replace_all(t, "{{TASK}}",          task);
    t = sol_replace_all(t, "{{DATASET_NAME}}",  dsName);
    t = sol_replace_all(t, "{{DATASET_SPLIT}}", dsSplit);
    t = sol_replace_all(t, "{{EPOCHS}}",        to_string(epochs));
    t = sol_replace_all(t, "{{LR}}",            string(lrBuf));
    t = sol_replace_all(t, "{{BATCH_SIZE}}",    to_string(batchSize));
    t = sol_replace_all(t, "{{OUTPUT_DIR}}",    outputDir);
    t = sol_replace_all(t, "{{HF_TOKEN}}",      hfTok);
    t = sol_replace_all(t, "{{UPLOAD_URL}}",    uploadUrlEnv ? string(uploadUrlEnv) : "");
    t = sol_replace_all(t, "{{S3_KEY}}",        s3KeyEnv     ? string(s3KeyEnv)     : "");
    return t;
}

static string generateSlurmScript(const string& jobId, const string& jobDir)
{
    string t = R"(#!/bin/bash
#SBATCH --job-name=nova_{{JOB_ID}}
#SBATCH --output={{JOB_DIR}}/slurm_output.txt
#SBATCH --error={{JOB_DIR}}/slurm_error.txt
#SBATCH --time=02:00:00
#SBATCH --partition=general
#SBATCH --gres=gpu:1
#SBATCH --mem=16G
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4

cd {{JOB_DIR}}
module load cuda/12.9
module load python/3.10
# Only install if not already present (avoids ~60s overhead on every job)
python -c "import torch, transformers, datasets, accelerate, safetensors" 2>/dev/null || \
    pip install --quiet torch transformers datasets accelerate safetensors --user
python train.py
echo "NOVA_JOB_DONE:$?"
)";
    t = sol_replace_all(t, "{{JOB_ID}}",  jobId);
    t = sol_replace_all(t, "{{JOB_DIR}}", jobDir);
    return t;
}

// ── HF helpers ────────────────────────────────────────────────────────────────

static string hf_escape_json(const string& s)
{
    string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static string hf_extract_text(const string& json)
{
    if (json.empty()) return "[HF Error: empty response — check token or network]";
    // Detect HTML error page (proxy, firewall, or wrong endpoint)
    if (json.find("<!DOCTYPE") != string::npos || json.find("<html") != string::npos)
        return "[HF Error: received HTML instead of JSON — check EC2 outbound access to api-inference.huggingface.co]";

    auto find_field = [&](const string& field) -> string {
        string key = "\"" + field + "\"";
        size_t pos = json.find(key);
        if (pos == string::npos) return "";
        pos = json.find(':', pos + key.size());
        if (pos == string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == string::npos) return "";
        size_t end = pos + 1;
        while (end < json.size() && json[end] != '"') {
            if (json[end] == '\\') end++;
            end++;
        }
        return json.substr(pos + 1, end - pos - 1);
    };
    // Check for JSON error field first
    string err = find_field("error");
    if (!err.empty()) return "[HF Error: " + err + "]";
    string r = find_field("generated_text");
    if (!r.empty()) return r;
    r = find_field("label");
    if (!r.empty()) return r;
    r = find_field("answer");
    if (!r.empty()) return r;
    return "[HF Error: unexpected response: " + json.substr(0, 120) + "]";
}

void execute_program(struct InstructionNode* program)
{
    struct InstructionNode* pc = program;
    int op1, op2, result;

    // Infinite loop detection:
    // Simple (non-tensor) instructions execute millions/sec in tight loops.
    // ML training with tensor ops executes only ~100K nodes for 500 epochs.
    // Limit: 500M simple instructions before aborting.
    // Each TENSOR_CALL grants +10M extra budget (rewards real ML work).
    static const long long SIMPLE_LIMIT = 500'000'000LL;
    static const long long TENSOR_BONUS =  10'000'000LL;
    long long simple_budget = SIMPLE_LIMIT;

    while (pc != NULL)
    {
        // Debug mode pause
        if (debug_mode && pc->line_no > 0 && pc->line_no > import_line_offset)
        {
            bool should_pause = debug_step;
            if (!should_pause && debug_breakpoints.count(pc->line_no - import_line_offset))
            {
                should_pause = true;
            }
            if (should_pause)
            {
                debug_print_state(pc->line_no - import_line_offset);
                string cmd = debug_wait_command();
                if (cmd == "stop")
                {
                    printf("__DEBUG__{\"type\":\"stopped\"}\n");
                    fflush(stdout);
                    return;
                }
                else if (cmd == "continue")
                {
                    debug_step = false;
                }
            }
        }
        // Decrement budget for every instruction.
        // TENSOR_CALL grants a large bonus so ML programs are never falsely killed.
        if (pc->type == TENSOR_CALL)
            simple_budget += TENSOR_BONUS;
        else
            simple_budget--;

        if (simple_budget <= 0)
        {
            fprintf(stderr,
                "\nError: execution limit reached (~500M instructions). "
                "Check for an infinite loop.\n"
                "Tip: ML training programs won't hit this limit — "
                "each tensor op grants extra budget.\n");
            exit(1);
        }

        switch (pc->type)
        {
            case NOOP:
                pc = pc->next;
                break;

            case IN:
            {
                int val = 0;
                if (input_replay_index >= 0)
                {
                    // ── replay mode (benchmark) ───────────────────────────────
                    if (input_replay_index < (int)input_replay_buffer.size())
                        val = input_replay_buffer[input_replay_index++];
                }
                else
                {
                    // ── live stdin mode ───────────────────────────────────────
                    scanf("%d", &val);

                    // capture for potential benchmark replay
                    input_replay_buffer.push_back(val);
                }
                mem[pc->input_inst.var_index] = val;
                pc = pc->next;
                break;
            }

            case OUT:
                if (!suppress_output)
                {
                    if (pc->output_inst.is_string)
                    {
                        // is_string_var: var_index is a mem slot holding a strMem index
                        // !is_string_var: var_index is a direct strMem index (literal)
                        int strIdx = pc->output_inst.is_string_var ? mem[pc->output_inst.var_index] : pc->output_inst.var_index;
                        printf("%s", strMem[strIdx].c_str());
                    }
                    else if (pc->output_inst.value_type == TYPE_FLOAT)
                    {
                        printf("%f", fmem[pc->output_inst.var_index]);
                    }
                    else if (pc->output_inst.value_type == TYPE_DOUBLE)
                    {
                        printf("%lf", dmem[pc->output_inst.var_index]);
                    }
                    else
                    {
                        printf("%d", mem[pc->output_inst.var_index]);
                    }
                    if (pc->output_inst.newline)
                        printf("\n");
                    else
                        printf(" ");
                    fflush(stdout);
                }
                pc = pc->next;
                break;

            case ASSIGN:
                switch (pc->assign_inst.op)
                {
                    case OPERATOR_PLUS:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 + op2;
                        break;
                    case OPERATOR_MINUS:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 - op2;
                        break;
                    case OPERATOR_MULT:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 * op2;
                        break;
                    case OPERATOR_DIV:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 / op2;
                        break;
                    case OPERATOR_NONE:
                        op1 = mem[pc->assign_inst.operand1_index];
                        result = op1;
                        break;
                }
                mem[pc->assign_inst.left_hand_side_index] = result;
                pc = pc->next;
                break;

            case CJMP:
                if (pc->cjmp_inst.target == NULL)
                {
                    debug("Error: pc->cjmp_inst->target is null.\n");
                    exit(1);
                }
                op1 = mem[pc->cjmp_inst.operand1_index];
                op2 = mem[pc->cjmp_inst.operand2_index];
                switch (pc->cjmp_inst.condition_op)
                {
                    case CONDITION_GREATER:
                        pc = (op1 > op2) ? pc->next : pc->cjmp_inst.target;
                        break;
                    case CONDITION_LESS:
                        pc = (op1 < op2) ? pc->next : pc->cjmp_inst.target;
                        break;
                    case CONDITION_NOTEQUAL:
                        pc = (op1 != op2) ? pc->next : pc->cjmp_inst.target;
                        break;
                }
                break;

            case JMP:
                if (pc->jmp_inst.target == NULL)
                {
                    debug("Error: pc->jmp_inst->target is null.\n");
                    exit(1);
                }
                pc = pc->jmp_inst.target;
                break;

            case CALL:
            {
                if (pc->call_inst.function_head == NULL)
                {
                    debug("Error: CALL target function_head is null.\n");
                    exit(1);
                }

                // JIT hot path check
                InstructionNode* fhead = pc->call_inst.function_head;
                jit_call_counts[fhead]++;

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

                // Fast path: native JIT call (skip if previously deoptimized)
                if (!jit_deoptimized.count(fhead) && jit_compiled.count(fhead) && jit_compiled[fhead] != nullptr)
                {
                    typedef int64_t (*JitFunc)(int64_t);
                    JitFunc fn = (JitFunc)jit_compiled[fhead];
                    int argVal = mem[pc->call_inst.arg_val_slots[0]];
                    bool deopt = false;
                    int64_t result = 0;

                    #ifdef _MSC_VER
                    __try
                    {
                        result = fn((int64_t)argVal);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        deopt = true;
                    }
                    #else
                    result = fn((int64_t)argVal);
                    if (result == INT64_MIN)
                        deopt = true;
                    #endif

                    if (deopt)
                    {
                        fprintf(stderr, "JIT: deopt function, falling back to interpreter\n");
                        jit_deoptimized.insert(fhead);
                        jit_compiled[fhead] = nullptr;
                        // Fall through to interpreter path below - do NOT break
                    }
                    else
                    {
                        mem[pc->call_inst.ret_val_index] = (int)result;
                        pc = pc->next;
                        break;
                    }
                }
                else if (jit_call_counts[fhead] >= JIT_THRESHOLD && !jit_compiled.count(fhead)
                         && !jit_deoptimized.count(fhead)
                         && pc->call_inst.num_params <= 1)  // JIT only handles single-param functions
                {
#ifdef _WIN32
                    jit_compiled[fhead] = jit_compile_function(fhead);

                    if (!jit_compiled[fhead])
                    {
                        jit_compiled[fhead] = nullptr;
                    }
#endif
                }
                
                // Interpreter fallback path
                CallFrame frame;
                frame.returnAddress  = pc->next;
                frame.dest_index     = pc->call_inst.ret_val_index;
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
                }

                int base  = pc->call_inst.func_slot_base;
                int count = pc->call_inst.func_slot_count;
                for (int i = 0; i < count; i++)
                    frame.savedSlots.push_back(mem[base + i]);

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
                }

                callStack.push(frame);
                pc = pc->call_inst.function_head;
                break;
            }

            case RET:
            {
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
                vector<int> capturedRetVals;
                for (int slot : frame.allRetSlots)
                {
                    capturedRetVals.push_back(mem[slot]);
                }
                // if single return, fall back to ret_inst
                if (capturedRetVals.empty())
                {
                    capturedRetVals.push_back(mem[pc->ret_inst.ret_val_index]);
                }

                // Populate memo cache for pure recursive functions on first computation
                if (frame.is_memoizable && frame.memo_func)
                {
                    memo_cache[frame.memo_func][frame.memo_key] = capturedRetVals;
                }

                // restore function-local slots
                for (int i = 0;i < (int)frame.savedSlots.size();i++)
                {
                    mem[frame.slot_base + i] = frame.savedSlots[i];
                }

                // write all return values back
                for (int i = 0;i < (int)capturedRetVals.size() && i < (int)frame.allRetSlots.size();i++)
                {
                    mem[frame.allRetSlots[i]] = capturedRetVals[i];
                }
                //backward compatibility - also to dst_index
                mem[frame.dest_index] = capturedRetVals[0];

                // free all function-local slots EXCEPT dest_index
                // dest_index must survive until the callers ASSIGN copies to it
                // that ASSIGN emits no alloc_slot calls, so dest_index is dafe
                int count = (int)frame.savedSlots.size();
                for (int i = 0;i < count;i++)
                {
                    int slot = frame.slot_base + i;
                    if (slot != frame.dest_index)
                    {
                        free_slot(slot);
                    }
                }
                pc = frame.returnAddress;
                break;
            }

            case ARRAY_READ:
            {
                int base = pc->array_inst.dynamic_base
                           ? mem[pc->array_inst.base_index]
                           : pc->array_inst.base_index;
                int size = (pc->array_inst.size_slot >= 0)
                           ? mem[pc->array_inst.size_slot]
                           : pc->array_inst.array_size;
                int idx  = mem[pc->array_inst.index_slot];
                if (idx < 0 || idx >= size)
                {
                    printf("Runtime error at line %d: array index %d out of bounds (size %d)\n", pc->array_inst.line_no, idx, size);
                    exit(1);
                }
                mem[pc->array_inst.target_index] = mem[base + idx];
                pc = pc->next;
                break;
            }

            case ARRAY_WRITE:
            {
                int base = pc->array_inst.dynamic_base
                           ? mem[pc->array_inst.base_index]
                           : pc->array_inst.base_index;
                int size = (pc->array_inst.size_slot >= 0)
                           ? mem[pc->array_inst.size_slot]
                           : pc->array_inst.array_size;
                int idx  = mem[pc->array_inst.index_slot];
                if (idx < 0 || idx >= size)
                {
                    printf("Runtime error at line %d: array index %d out of bounds (size %d)\n",
                           pc->array_inst.line_no, idx, pc->array_inst.array_size);
                    exit(1);
                }
                mem[base + idx] = mem[pc->array_inst.target_index];
                pc = pc->next;
                break;
            }

            case ALLOC:
            {
                int size = mem[pc->alloc_inst.size_slot];
                if (size <= 0)
                {
                    printf("Runtime error: array size must > 0, got %d\n", size);
                    exit(1);
                }
                int base = next_available;
                for (int i = 0;i < size;i++)
                {
                    alloc_slot();
                }
                mem[pc->alloc_inst.base_slot] = base;
                pc = pc->next;
                break;
            }

            case STRCAT:
            {
                int leftIdx = mem[pc->strcat_inst.left_slot];
                int rightIdx = mem[pc->strcat_inst.right_slot];
                strMem.push_back(strMem[leftIdx] + strMem[rightIdx]);
                int dest = next_str_available++;
                mem[pc->strcat_inst.dest_slot] = dest;
                pc = pc->next;
                break;
            }

            case SCMP:
            {
                int leftIdx = mem[pc->scmp_inst.operand1_index];
                int rightIdx = mem[pc->scmp_inst.operand2_index];
                string& left = strMem[leftIdx];
                string& right = strMem[rightIdx];
                bool pass = false;
                switch (pc->scmp_inst.condition_op)
                {
                    case CONDITION_GREATER: pass = (left > right); break;
                    case CONDITION_LESS: pass = (left < right); break;
                    case CONDITION_NOTEQUAL: pass = (left != right); break;
                }
                pc = pass ? pc->next : pc->scmp_inst.target;
                break;
            }

            case ASSIGN_F:
            {
                float op1f, op2f, resultf;
                switch (pc->assign_f_inst.op)
                {
                    case OPERATOR_PLUS: 
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] + fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MINUS:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] - fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MULT:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] * fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_DIV:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index] / fmem[pc->assign_f_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_NONE:
                    {
                        resultf = fmem[pc->assign_f_inst.operand1_index];
                        break;
                    }
                }
                fmem[pc->assign_f_inst.left_hand_side_index] = resultf;
                pc = pc->next;
                break;
            }

            case ASSIGN_D:
            {
                // fprintf(stderr, "ASSIGN_D: left=%d op1=%d op=%d dmem.size=%d\n",
                //     pc->assign_d_inst.left_hand_side_index,
                //     pc->assign_d_inst.operand1_index,
                //     (int)pc->assign_d_inst.op,
                //     (int)dmem.size());
                double op1d, op2d, resultd;
                switch (pc->assign_d_inst.op)
                {
                    case OPERATOR_PLUS:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] + dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MINUS:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] - dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_MULT:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] * dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_DIV:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index] / dmem[pc->assign_d_inst.operand2_index];
                        break;
                    }
                    case OPERATOR_NONE:
                    {
                        resultd = dmem[pc->assign_d_inst.operand1_index];
                        break;
                    }
                } 
                dmem[pc->assign_d_inst.left_hand_side_index] = resultd;
                pc = pc->next;
                break;
            }

            case CAST:
            {
                int si = pc->cast_inst.src_index;
                int di = pc->cast_inst.dst_index;
                VarType st = pc->cast_inst.src_type;
                VarType dt = pc->cast_inst.dst_type;

                double val = 0.0;
                if (st == TYPE_INT) val = (double)mem[si];
                else if (st == TYPE_FLOAT) val = (float)fmem[si];
                else if (st == TYPE_DOUBLE) val = (double)dmem[si];

                if (dt == TYPE_INT) mem[di] = (int)val;
                else if (dt == TYPE_FLOAT) fmem[di] = (float)val;
                else if (dt == TYPE_DOUBLE) dmem[di] = (double)val;

                pc = pc->next;
                break;
            }

            case TENSOR_CALL:
            {
                auto& tc = pc->tensor_call_inst;
                int* a = tc.arg_slots;
                int res = 0;

                // Returns the scalar double value for arg i, regardless of whether it's
                // an int (mem[]), float (fmem[]), or double (dmem[]) arg.
                auto getScalar = [&](int i) -> double {
                    switch (tc.arg_types[i]) {
                        case TYPE_FLOAT:  return (double)fmem[a[i]];
                        case TYPE_DOUBLE: return dmem[a[i]];
                        default:          return (double)mem[a[i]];
                    }
                };

                switch (tc.op)
                {
                    case TEN_ALLOC:
                    {
                        res = alloc_tensor(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_ZEROS:
                    {
                        res = ten_zeros(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_ONES:
                    {
                        res = ten_ones(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_RANDN:
                    {
                        res = ten_randn(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_XAVIER:
                    {
                        res = ten_xavier(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_GET:
                    {
                        // tensor_get returns a double - store in dmem, but result_slot in a mem[] index
                        // we store it as int (truncated) for now; full float support added later
                        res = (int)tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]);
                        break;
                    }
                    case TEN_SET:
                    {
                        tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]) = getScalar(3);
                        res = 0;
                        break;
                    }
                    case TEN_ROWS:
                    {
                        res = tensor_heap[mem[a[0]]].rows;
                        break;
                    }
                    case TEN_COLS:
                    {
                        res = tensor_heap[mem[a[0]]].cols;
                        break;
                    }
                    case TEN_PRINT:
                    {
                        Tensor& T = tensor_heap[mem[a[0]]];
                        // Build entire output as single string then print once
                        string out_str = "";
                        out_str += "[";
                        for (int i = 0; i < T.rows; i++)
                        {
                            if (T.rows > 1) out_str += (i == 0 ? "[" : " [");
                            for (int j = 0; j < T.cols; j++)
                            {
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%.4f", T.at(i, j));
                                out_str += buf;
                                if (j < T.cols - 1) out_str += ", ";
                            }
                            if (T.rows > 1) out_str += (i < T.rows - 1 ? "],\n" : "]");
                        }
                        out_str += "]\n";
                        printf("%s", out_str.c_str());
                        fflush(stdout);
                        res = 0;
                        break;
                    }
                    case TEN_MATMUL: 
                    {
                        if (grad_mode)
                            res = ag_matmul(mem[a[0]], mem[a[1]]);
                        else
                            res = ten_matmul(mem[a[0]], mem[a[1]]); 
                        break;
                    }
                    case TEN_ADD: 
                    {
                        if (grad_mode)
                            res = ag_add(mem[a[0]], mem[a[1]]);
                        else
                            res = ten_add(mem[a[0]], mem[a[1]]); 
                        break;
                    }
                    case TEN_SUB:
                    { 
                        if (grad_mode)
                            res = ag_sub(mem[a[0]], mem[a[1]]);
                        else
                            res = ten_sub(mem[a[0]], mem[a[1]]); 
                        break;
                    }
                    case TEN_MUL: 
                    {
                        if (grad_mode)
                            res = ag_mul(mem[a[0]], mem[a[1]]);
                        else
                            res = ten_mul(mem[a[0]], mem[a[1]]); 
                        break;
                    }
                    case TEN_SCALE: 
                    {
                        if (grad_mode)
                            res = ag_scale(mem[a[0]], getScalar(1));
                        else
                            res = ten_scale(mem[a[0]], getScalar(1)); 
                        break;
                    }
                    case TEN_T: 
                    {
                        if (grad_mode)
                            res = ag_transpose(mem[a[0]]);
                        else
                            res = ten_transpose(mem[a[0]]); 
                        break;
                    }
                    case TEN_SUM:
                    {
                        double s = 0.0;
                        for (auto v : tensor_heap[mem[a[0]]].data)
                        {
                            s += v;
                        }
                        int dslot = (int)dmem.size();
                        dmem.push_back(s);
                        next_double_available++;
                        res = dslot;
                        break;
                    }
                    case TEN_MEAN:
                    {
                        Tensor& T = tensor_heap[mem[a[0]]];
                        double s = 0.0;
                        for (auto v : T.data)
                        {
                            s += v;
                        }
                        int dslot = (int)dmem.size();
                        dmem.push_back(s / T.data.size());
                        next_double_available++;
                        res = dslot;
                        break;
                    }
                    case TEN_MAX:
                    {
                        Tensor& T = tensor_heap[mem[a[0]]];
                        double mx = T.data[0];
                        for (auto v : T.data)
                        {
                            mx = std::max(mx, v);
                        }
                        int dslot = (int)dmem.size();
                        dmem.push_back(mx);
                        next_double_available++;
                        res = dslot;
                        break;
                    }
                    case TEN_CLIP: res = ten_clip(mem[a[0]], getScalar(1), getScalar(2)); break;
                    case TEN_RELU:
                    {
                        if (grad_mode)
                            res = ag_relu(mem[a[0]]);
                        else 
                            res = ten_relu(mem[a[0]]); 
                        break;
                    }
                    case TEN_SIGMOID: 
                    {
                        if (grad_mode)
                            res = ag_sigmoid(mem[a[0]]);
                        else
                            res = ten_sigmoid(mem[a[0]]); 
                        break;
                    }
                    case TEN_TANH:
                    { 
                        if (grad_mode)
                            res = ag_tanh_t(mem[a[0]]);
                        else
                            res = ten_tanh(mem[a[0]]); 
                        break;
                    }
                    case TEN_SOFTMAX: 
                    {
                        if (grad_mode)
                            res = ag_softmax(mem[a[0]]);
                        else
                            res = ten_softmax(mem[a[0]]); 
                        break;
                    }
                    case TEN_RELU_GRAD: res = ten_relu_grad(mem[a[0]]); break;
                    case TEN_SIGMOID_GRAD: res = ten_sigmoid_grad(mem[a[0]]); break;
                    case TEN_MSE_LOSS:
                    {
                        if (grad_mode)
                        {
                            res = ag_mse_loss(mem[a[0]], mem[a[1]]);
                        }
                        else
                        {
                            double loss = ten_mse_loss(mem[a[0]], mem[a[1]]);
                            int dslot = (int)dmem.size();
                            dmem.push_back(loss);
                            next_double_available++;
                            res = dslot;
                        }
                        break;
                    }
                    case TEN_MSE_GRAD: res = ten_mse_grad(mem[a[0]], mem[a[1]]); break;
                    case TEN_BCE_GRAD: res = ten_bce_grad(mem[a[0]], mem[a[1]]); break;
                    case TEN_CE_LOSS:
                    {
                        if (grad_mode)
                        {
                            res = ag_ce_loss(mem[a[0]], mem[a[1]]);
                        }
                        else
                        {
                            double loss = ten_ce_loss(mem[a[0]], mem[a[1]]);
                            int dslot = (int)dmem.size();
                            dmem.push_back(loss);
                            next_double_available++;
                            res = dslot;
                        }
                        break;
                    }
                    case TEN_CE_GRAD:    res = ten_ce_grad(mem[a[0]], mem[a[1]]); break;
                    case TEN_SUM_AXIS0: 
                    {
                        if (grad_mode)
                            res = ag_sum_axis0(mem[a[0]]);
                        else
                            res = ten_sum_axis0(mem[a[0]]); 
                        break;
                    }
                    case TEN_RAND_INT:
                    {
                        res = ten_rand_int(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_GET_ROW: res = ten_get_row(mem[a[0]], mem[a[1]]); break;
                    case TEN_SET_ROW:
                    {
                        ten_set_row(mem[a[0]], mem[a[1]], mem[a[2]]);
                        res = 0;
                        break;
                    }
                    case TEN_SLICE_ROWS: res = ten_slice_rows(mem[a[0]], mem[a[1]], mem[a[2]]); break;
                    case TEN_LOAD_CSV:
                    {
                        int strIdx = mem[a[0]];
                        res = ten_load_csv(strMem[strIdx], mem[a[1]], mem[a[2]]);
                        if (res < 0) { fprintf(stderr, "tensor_load_csv failed\n"); exit(1); }
                        break;
                    }
                    case TEN_REQUIRES_GRAD:
                    {
                        requires_grad(mem[a[0]]);
                        res = 0;
                        break;
                    }
                    case TEN_BACKWARD:
                    {
                        backward(mem[a[0]]);
                        res = 0;
                        break;
                    }
                    case TEN_TENSOR_GRAD:
                    {
                        res = tensor_grad(mem[a[0]]);
                        break;
                    }
                    case TEN_ZERO_GRAD:
                    {
                        zero_grad(mem[a[0]]);
                        res = 0;
                        break;
                    }
                    case TEN_GRAD_STEP:
                    {
                        res = grad_step(mem[a[0]], getScalar(1));
                        break;
                    }
                }
                if (tc.result_slot >= 0) mem[tc.result_slot] = res;
                pc = pc->next;
                break;
            }

            case HF_INFER:
            {
                auto& hf = pc->hf_inst;
                string model = strMem[mem[hf.model_slot]];

                if (hf.op == HF_OP_SET_TOKEN) {
                    hf_api_token = model;
                    pc = pc->next;
                    break;
                }

                if (hf.op == HF_OP_DATASET) {
                    // model_slot holds dataset name, input_slot holds split string
                    string dsName  = model;
                    string dsSplit = (hf.input_slot >= 0) ? strMem[mem[hf.input_slot]] : "train";
                    string handle  = dsName + ":" + dsSplit;
                    int strIdx = (int)strMem.size();
                    strMem.push_back(handle);
                    if (hf.result_slot >= 0) mem[hf.result_slot] = strIdx;
                    pc = pc->next;
                    break;
                }

                // Trim any whitespace/newlines the parser may have left in the model name
                while (!model.empty() && (model.back() == ' ' || model.back() == '\n' ||
                       model.back() == '\r' || model.back() == '\t')) model.pop_back();
                while (!model.empty() && (model.front() == ' ' || model.front() == '\n' ||
                       model.front() == '\r' || model.front() == '\t')) model = model.substr(1);

                string tok = hf_api_token;
                if (tok.empty()) {
                    const char* env = getenv("HF_API_TOKEN");
                    if (env) tok = env;
                }
                // Trim token too
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\n' ||
                       tok.back() == '\r' || tok.back() == '\t')) tok.pop_back();

                string input = (hf.input_slot >= 0) ? strMem[mem[hf.input_slot]] : "";

                string payload;
                if (hf.op == HF_OP_GENERATE) {
                    payload = "{\"inputs\":\"" + hf_escape_json(input) +
                              "\",\"parameters\":{\"max_new_tokens\":" +
                              to_string(hf.max_tokens) + "}}";
                } else {
                    payload = "{\"inputs\":\"" + hf_escape_json(input) + "\"}";
                }

                string jobTag      = to_string((long long)(intptr_t)pc);
                string payloadFile = "/tmp/hf_payload_" + jobTag + ".json";
                string tmpOut      = "/tmp/hf_resp_"    + jobTag + ".json";
                string pyFile      = "/tmp/hf_req_"     + jobTag + ".py";

                // Write the JSON payload to a file — avoids all shell-quoting issues.
                { ofstream pf(payloadFile); pf << payload; }

                // Use Python urllib with:
                //   ProxyHandler({})  — disables all env-based proxies
                //   NoRedirect        — catches HTTP redirects before they reach HTML pages.
                //                       HF redirects requests with invalid/missing tokens
                //                       to huggingface.co (HTML) instead of returning JSON.
                string pyScript =
                    "import urllib.request, urllib.error, json, sys\n"
                    "\n"
                    "class NoRedirect(urllib.request.HTTPRedirectHandler):\n"
                    "    def redirect_request(self, req, fp, code, msg, headers, newurl):\n"
                    "        raise Exception('HF API redirected ' + str(code) + ' to: ' + newurl[:80]\n"
                    "                        + ' — check your HF token is valid')\n"
                    "\n"
                    "with open('" + payloadFile + "', 'rb') as f: data = f.read()\n"
                    "url = 'https://api-inference.huggingface.co/models/" + model + "'\n"
                    "req = urllib.request.Request(url, data=data, method='POST')\n"
                    "req.add_header('Authorization', 'Bearer " + tok + "')\n"
                    "req.add_header('Content-Type', 'application/json')\n"
                    "opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect)\n"
                    "try:\n"
                    "    with opener.open(req, timeout=30) as r:\n"
                    "        sys.stdout.write(r.read().decode('utf-8'))\n"
                    "except urllib.error.HTTPError as e:\n"
                    "    try: sys.stdout.write(e.read().decode('utf-8'))\n"
                    "    except: sys.stdout.write(json.dumps({'error': 'HTTP ' + str(e.code)}))\n"
                    "except Exception as e:\n"
                    "    sys.stdout.write(json.dumps({'error': str(e)}))\n";

                { ofstream pf(pyFile); pf << pyScript; }

                string pyCmd = "python3 " + pyFile + " > " + tmpOut + " 2>/dev/null";
                fprintf(stderr, "HF_CMD: python3 %s\n", pyFile.c_str());
                system(pyCmd.c_str());

                string response;
                {
                    ifstream rf(tmpOut);
                    response.assign(istreambuf_iterator<char>(rf),
                                    istreambuf_iterator<char>());
                }
                fprintf(stderr, "HF_RESP: %s\n", response.substr(0, 200).c_str());
                remove(payloadFile.c_str());
                remove(pyFile.c_str());
                remove(tmpOut.c_str());

                if (hf.result_slot >= 0) {
                    string text = hf_extract_text(response);
                    int strIdx = (int)strMem.size();
                    strMem.push_back(text);
                    mem[hf.result_slot] = strIdx;
                }

                pc = pc->next;
                break;
            }

            case SOL_CALL:
            {
                auto& si = pc->sol_inst;

                switch (si.op)
                {
                    case SOL_OP_SET_KEY:
                    {
                        // sol.set_key() is disabled — SSH key is configured server-side
                        // via SOL_PRIVATE_KEY_PATH to prevent users from accessing SOL directly.
                        break;
                    }

                    case SOL_OP_FINETUNE:
                    {
                        string model   = strMem[mem[si.model_slot]];
                        string task    = strMem[mem[si.task_slot]];
                        string dataset = (si.dataset_slot >= 0) ? strMem[mem[si.dataset_slot]] : "imdb:train";

                        // Parse "name:split" handle produced by hf.dataset()
                        string dsName = dataset, dsSplit = "train";
                        size_t sep = dataset.find(':');
                        if (sep != string::npos) {
                            dsName  = dataset.substr(0, sep);
                            dsSplit = dataset.substr(sep + 1);
                        }

                        string jobId  = to_string((long long)time(nullptr)) + "_" +
                                        to_string((unsigned)rand() % 10000);
                        string jobDir = sol_work_dir + "/" + jobId;

                        string trainPy = generateTrainScript(model, task, dsName, dsSplit,
                                                             si.epochs, si.lr, si.batch_size,
                                                             jobDir, hf_api_token);
                        string slurmSh = generateSlurmScript(jobId, jobDir);

                        string localPy    = "/tmp/train_" + jobId + ".py";
                        string localSlurm = "/tmp/job_"   + jobId + ".slurm";
                        sol_write_file(localPy,    trainPy);
                        sol_write_file(localSlurm, slurmSh);

                        sshExecSOL("mkdir -p " + jobDir);
                        sftpUploadSOL(localPy,    jobDir + "/train.py");
                        sftpUploadSOL(localSlurm, jobDir + "/job.slurm");

                        string sbatchOut  = sshExecSOL("sbatch " + jobDir + "/job.slurm");
                        string slurmJobId = sol_extract_slurm_id(sbatchOut);

                        if (slurmJobId.empty()) {
                            // sbatch failed or returned unexpected output — surface it to the user
                            printf("Nova: SOL sbatch failed. Output: %s\n",
                                   sbatchOut.empty() ? "(empty — tunnel may be down)" : sbatchOut.c_str());
                            // Use sentinel so the wait loop exits immediately
                            slurmJobId = "0";
                        }

                        printf("Nova: SOL finetune submitted — SLURM job ID %s\n", slurmJobId.c_str());

                        // Handle: "jobId:slurmJobId:jobDir:model"
                        string handle = jobId + ":" + slurmJobId + ":" + jobDir + ":" + model;
                        int strIdx = (int)strMem.size();
                        strMem.push_back(handle);
                        mem[si.job_slot] = strIdx;

                        remove(localPy.c_str());
                        remove(localSlurm.c_str());
                        break;
                    }

                    case SOL_OP_WAIT:
                    {
                        string handle = strMem[mem[si.job_slot]];
                        // Parse handle: "jobId:slurmJobId:jobDir:model"
                        size_t c1 = handle.find(':');
                        size_t c2 = (c1 != string::npos) ? handle.find(':', c1 + 1) : string::npos;
                        size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                        if (c1 == string::npos) { pc = pc->next; break; }
                        string slurmJobId = handle.substr(c1 + 1, c2 - c1 - 1);

                        if (slurmJobId == "0") {
                            printf("Nova: Skipping wait — job was not submitted.\n");
                            pc = pc->next;
                            break;
                        }

                        string jobDir_wait = (c3 != string::npos)
                            ? handle.substr(c2 + 1, c3 - c2 - 1)
                            : handle.substr(c2 + 1);

                        bool seenRunning = false;
                        int  runPolls    = 0;
                        string lastSnippet;

                        while (true) {
                            string status = sol_trim(sshExecSOL(
                                "squeue -j " + slurmJobId + " -h -o \"%T\" 2>/dev/null || echo DONE"));

                            if (status.empty() || status == "DONE" || status == "COMPLETED") {
                                printf("Nova: Training complete!\n");
                                break;
                            } else if (status == "FAILED" || status == "CANCELLED") {
                                fprintf(stderr, "Nova: Job %s %s on SOL cluster.\n",
                                        slurmJobId.c_str(), status.c_str());
                                break;
                            } else if (status == "PENDING" || status == "CONFIGURING") {
                                printf("Nova: Training job in queue, waiting for GPU server...\n");
                            } else if (status == "RUNNING") {
                                if (!seenRunning) {
                                    printf("Nova: GPU server acquired! Training started.\n");
                                    seenRunning = true;
                                    runPolls = 0;
                                }
                                // Stream a live snippet of the training log every poll (60s)
                                if (runPolls % 1 == 0) {
                                    string snippet = sol_trim(sshExecSOL(
                                        "tail -2 " + jobDir_wait + "/slurm_output.txt 2>/dev/null"));
                                    if (!snippet.empty() && snippet != lastSnippet) {
                                        printf("%s\n", snippet.c_str());
                                        lastSnippet = snippet;
                                    }
                                }
                                runPolls++;
                            }
#ifndef _WIN32
                            sleep(60);
#endif
                        }

                        // Show final training summary (metrics + upload confirmation)
                        string slurmLog = sol_trim(sshExecSOL(
                            "grep -v 'it/s\\]' " + jobDir_wait +
                            "/slurm_output.txt 2>/dev/null | tail -15"));
                        if (!slurmLog.empty())
                            printf("Nova: [Training summary]\n%s\n", slurmLog.c_str());

                        // Show any errors from SLURM stderr
                        string slurmErr = sol_trim(sshExecSOL(
                            "tail -10 " + jobDir_wait + "/slurm_error.txt 2>/dev/null"));

                        break;
                    }

                    case SOL_OP_PREDICT:
                    {
                        string handle = strMem[mem[si.job_slot]];
                        size_t c2 = handle.find(':', handle.find(':') + 1);
                        size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                        string jobDir = (c3 != string::npos)
                            ? handle.substr(c2 + 1, c3 - c2 - 1)
                            : handle.substr(c2 + 1);
                        string input  = strMem[mem[si.input_slot]];

                        // Escape input for embedding in Python string literal
                        string escaped;
                        for (char c : input) {
                            if (c == '"') escaped += "\\\"";
                            else if (c == '\\') escaped += "\\\\";
                            else escaped += c;
                        }

                        string inferPy =
                            "import sys, os, json\n"
                            "job_dir = '" + jobDir + "'\n"
                            "if not os.path.isdir(job_dir):\n"
                            "    print(f'[SOL predict] Job directory not found: {job_dir}')\n"
                            "    sys.exit(0)\n"
                            "if 'config.json' not in os.listdir(job_dir):\n"
                            "    print('[SOL predict] config.json missing — training likely failed')\n"
                            "    sys.exit(0)\n"
                            "try:\n"
                            "    import torch\n"
                            "    from transformers import AutoTokenizer, AutoModelForSequenceClassification\n"
                            "    tokenizer = AutoTokenizer.from_pretrained(job_dir, local_files_only=True)\n"
                            "    model = AutoModelForSequenceClassification.from_pretrained(\n"
                            "        job_dir, local_files_only=True, ignore_mismatched_sizes=True)\n"
                            "    model.eval()\n"
                            "    inputs = tokenizer(\"" + escaped + "\", return_tensors='pt',\n"
                            "                       truncation=True, padding=True, max_length=512)\n"
                            "    inputs.pop('token_type_ids', None)  # not all models use this\n"
                            "    with torch.no_grad():\n"
                            "        logits = model(**inputs).logits\n"
                            "    probs = torch.softmax(logits, dim=-1)[0]\n"
                            "    pred  = int(torch.argmax(probs).item())\n"
                            "    label = model.config.id2label.get(pred, f'LABEL_{pred}')\n"
                            "    score = float(probs[pred].item())\n"
                            "    print(f'{label} ({score:.3f})')\n"
                            "except Exception as e:\n"
                            "    print(f'[SOL predict error: {e}]')\n";

                        string localInfer  = "/tmp/infer_" + to_string((long long)(intptr_t)pc) + ".py";
                        string remoteInfer = jobDir + "/infer.py";
                        sol_write_file(localInfer, inferPy);
                        sftpUploadSOL(localInfer, remoteInfer);
                        remove(localInfer.c_str());

                        // Use bash -l (login shell) so module system init scripts are sourced.
                        // Direct SSH exec doesn't run /etc/profile.d/modules.sh, so
                        // `module load` would silently fail in a plain exec session.
                        string output = sol_trim(sshExecSOL(
                            "bash -l -c '"
                            "source /etc/profile.d/modules.sh 2>/dev/null || true; "
                            "module load python/3.10 2>/dev/null || true; "
                            "python3 " + remoteInfer + " 2>&1'"));
                        if (output.empty())
                            output = "[SOL predict: no output — model may not have saved correctly]";
                        int strIdx = (int)strMem.size();
                        strMem.push_back(output);
                        if (si.result_slot >= 0) mem[si.result_slot] = strIdx;
                        break;
                    }

                    case SOL_OP_SAVE:
                    {
                        string handle = strMem[mem[si.job_slot]];
                        size_t c1 = handle.find(':');
                        size_t c2 = (c1 != string::npos) ? handle.find(':', c1 + 1) : string::npos;
                        size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                        string jobId  = handle.substr(0, c1);
                        string jobDir = (c3 != string::npos) ? handle.substr(c2 + 1, c3 - c2 - 1) : handle.substr(c2 + 1);
                        string model  = (c3 != string::npos) ? handle.substr(c3 + 1) : "";
                        string savePath = strMem[mem[si.path_slot]];

                        // Check if the SLURM job uploaded the model to S3
                        string uploadedS3Key;
                        string slurmFullLog = sol_trim(sshExecSOL(
                            "cat " + jobDir + "/slurm_output.txt 2>/dev/null"));
                        const string UPLOAD_MARKER = "NOVA_MODEL_UPLOADED:";
                        size_t mpos = slurmFullLog.find(UPLOAD_MARKER);
                        if (mpos != string::npos) {
                            size_t end = slurmFullLog.find('\n', mpos + UPLOAD_MARKER.size());
                            uploadedS3Key = sol_trim(slurmFullLog.substr(
                                mpos + UPLOAD_MARKER.size(),
                                end == string::npos ? string::npos
                                                    : end - mpos - UPLOAD_MARKER.size()));
                        }

                        if (!uploadedS3Key.empty()) {
                            // Model was uploaded to S3 by the SLURM compute node
                            string fname = uploadedS3Key;
                            size_t sl = fname.rfind('/');
                            if (sl != string::npos) fname = fname.substr(sl + 1);
                            printf("Nova: Model saved to your file storage as '%s'\n", fname.c_str());
                            // Marker for server to register in DynamoDB (no upload needed)
                            printf("NOVA_S3_FILE:%s\n", uploadedS3Key.c_str());
                        } else {
                            // Fallback: write a small JSON reference — model stays on SOL scratch
                            string meta =
                                "{\n"
                                "  \"type\": \"hf_model_ref\",\n"
                                "  \"model\": \"" + model + "\",\n"
                                "  \"job_id\": \"" + jobId + "\",\n"
                                "  \"sol_path\": \"" + jobDir + "\"\n"
                                "}\n";

                            ofstream nbc(savePath);
                            nbc << meta;
                            nbc.close();

                            printf("Nova: Model reference saved to %s\n", savePath.c_str());
                            printf("Nova: Full model weights remain on SOL at %s\n", jobDir.c_str());
                            printf("NOVA_SAVE_FILE:%s\n", savePath.c_str());
                        }
                        break;
                    }
                }

                pc = pc->next;
                break;
            }

            default:
                debug("Error: invalid value for pc->type (%d).\n", pc->type);
                exit(1);
                break;
        }
    }
}

// Constant folding 

static vector<bool> constant_slots;

void mark_constants(struct InstructionNode* program)
{
    int n = next_available;
    vector<int> write_count(n, 0);
    constant_slots.assign(n, true);
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN)
            write_count[pc->assign_inst.left_hand_side_index]++;
        if (pc->type == IN)
            write_count[pc->input_inst.var_index]++;
        if (pc->type == ARRAY_READ)
            write_count[pc->array_inst.target_index]++;
        pc = pc->next;
    }

    for (int i = 0;i < n;i++)
    {
        if (write_count[i] > 1) constant_slots[i] = false;
    }

    pc = program;
    while (pc != nullptr)
    {
        if (pc->type == IN)
            constant_slots[pc->input_inst.var_index] = false;
        if (pc->type == ARRAY_READ)
            constant_slots[pc->array_inst.target_index] = false;
        if (pc->type == ASSIGN && pc->assign_inst.op == OPERATOR_NONE)
            if (!constant_slots[pc->assign_inst.operand1_index])
                constant_slots[pc->assign_inst.left_hand_side_index] = false;
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
            if (!constant_slots[pc->assign_inst.operand1_index] ||
                !constant_slots[pc->assign_inst.operand2_index])
                constant_slots[pc->assign_inst.left_hand_side_index] = false;
        pc = pc->next;
    }
}

int fold_pass(struct InstructionNode* program)
{
    int folds = 0;
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
        {
            int op1_idx = pc->assign_inst.operand1_index;
            int op2_idx = pc->assign_inst.operand2_index;
            int dst_idx = pc->assign_inst.left_hand_side_index;

            if (constant_slots[op1_idx] && constant_slots[op2_idx])
            {
                int op1 = mem[op1_idx], op2 = mem[op2_idx], result = 0;
                switch (pc->assign_inst.op)
                {
                    case OPERATOR_PLUS:  result = op1 + op2; break;
                    case OPERATOR_MINUS: result = op1 - op2; break;
                    case OPERATOR_MULT:  result = op1 * op2; break;
                    case OPERATOR_DIV:
                        if (op2 == 0) { pc = pc->next; continue; }
                        result = op1 / op2; break;
                    default: pc = pc->next; continue;
                }
                mem[dst_idx]                   = result;
                pc->assign_inst.op             = OPERATOR_NONE;
                pc->assign_inst.operand1_index = dst_idx;
                constant_slots[dst_idx]        = true;
                folds++;
            }
        }
        pc = pc->next;
    }
    return folds;
}

int constant_fold(struct InstructionNode* program)
{
    mark_constants(program);
    int total_folds = 0, folds = 0;
    do {
        folds = fold_pass(program);
        total_folds += folds;
    } while (folds > 0);
    return total_folds;
}

int remove_self_copies(struct InstructionNode* head)
{
    int removed = 0;
    struct InstructionNode* pc = head;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN &&
            pc->assign_inst.op == OPERATOR_NONE &&
            pc->assign_inst.left_hand_side_index == pc->assign_inst.operand1_index)
        {
            pc->type = NOOP;
            removed++;
        }
        pc = pc->next;
    }
    return removed;
}

int algaebric_simplify(struct InstructionNode* program)
{
    int simplified = 0;
    int n = (int)constant_slots.size();

    // pre-allocate a zero slot for zero-product results
    int zero_slot = alloc_slot();
    mem[zero_slot] = 0;

    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
        {
            int op1 = pc->assign_inst.operand1_index;
            int op2 = pc->assign_inst.operand2_index;
            int dst = pc->assign_inst.left_hand_side_index;

            bool op1_const = (op1 < n && constant_slots[op1]);
            bool op2_const = (op2 < n && constant_slots[op2]);
            int op1_val = op1_const ? mem[op1] : 0;
            int op2_val = op2_const ? mem[op2] : 0;

            ArithmeticOperatorType op = pc->assign_inst.op;
            bool did_simplify = true;

            if (op == OPERATOR_PLUS)
            {
                if (op2_const && op2_val == 0)  // x + 0 = x
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else if (op1_const && op1_val == 0)  // 0 + x = x
                {
                    pc->assign_inst.operand1_index = op2;
                }
                else 
                {
                    did_simplify = false;
                }
            }
            else if (op == OPERATOR_MINUS)
            {
                if (op2_const && op2_val == 0)  // x - 0 = x
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else if (op1 == op2)  // x - x = 0
                {
                    pc->assign_inst.operand1_index = zero_slot;
                }
                else
                {
                    did_simplify = false;
                }
            }
            else if (op == OPERATOR_MULT)
            {
                if (op2_const && op2_val == 1)  // x * 1 = x
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else if (op1_const && op1_val == 1)  // 1 * x = x
                {
                    pc->assign_inst.operand1_index = op2;
                }
                else if (op2_const && op2_val == 0)  // x * 0 = 0
                {
                    pc->assign_inst.operand1_index = zero_slot;
                }
                else if (op1_const && op1_val == 0)  // 0 * x = 0
                {
                    pc->assign_inst.operand1_index = zero_slot;
                }
                else
                {
                    did_simplify = false;
                }
            }
            else if (op == OPERATOR_DIV)
            {
                if (op2_const && op2_val == 1)
                {
                    pc->assign_inst.operand1_index = op1;
                }
                else 
                {
                    did_simplify = false;
                }
            }
            else
            {
                did_simplify = false;
            }

            if (did_simplify)
            {
                pc->assign_inst.op = OPERATOR_NONE;
                simplified++;
            }
        }
        pc = pc->next;
    }
    return simplified;
}

int copy_propogate(struct InstructionNode* program)
{
    int n = next_available;

    // count writes per slot since slots written more than once are undafe to propogate
    vector<int> write_count(n, 0);
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN)
        {
            write_count[pc->assign_inst.left_hand_side_index]++;
        }
        if (pc->type == IN)
        {
            write_count[pc->input_inst.var_index]++;
        }
        if (pc->type == ARRAY_READ)
        {
            write_count[pc->array_inst.target_index]++;
        }
        pc = pc->next;
    }

    // copy_map[slot] = slot it is a direct copy of
    vector<int> copy_map(n, -1);  // -1 = not a copy

    auto resolve = [&](int slot) -> int {
        if (slot < n && copy_map[slot] != -1) return copy_map[slot];
        return slot;
    };

    int propogated = 0;
    pc = program;
    while (pc != nullptr)
    {
        switch (pc->type)
        {
            case ASSIGN: 
            {
                int dst = pc->assign_inst.left_hand_side_index;
                if (pc->assign_inst.op == OPERATOR_NONE)
                {
                    // resolve src through map first
                    int src = resolve(pc->assign_inst.operand1_index);
                    pc->assign_inst.operand1_index = src;

                    // only record as a copy if slot is written exactly once
                    if (dst != src && dst < n && write_count[dst] == 1 && (src >= n || write_count[src] == 1))
                    {
                        copy_map[dst] = src;
                    }
                    else
                    {
                        copy_map[dst] = -1; // unsafe, so invalidate
                    }
                }
                else
                {
                    // arithmetic node - resolve both operands
                    int op1 = resolve(pc->assign_inst.operand1_index);
                    int op2 = resolve(pc->assign_inst.operand2_index);
                    if (op1 != pc->assign_inst.operand1_index) 
                    {
                        pc->assign_inst.operand1_index = op1;
                        propogated++;
                    }
                    if (op2 != pc->assign_inst.operand2_index)
                    {
                        pc->assign_inst.operand2_index = op2;
                    }
                    // dst is now computed - no longer a simple copy
                    if (dst < n) copy_map[dst] = -1;
                }
                break;
            }
            case IN:
                // runtime input - invalidate
                if (pc->input_inst.var_index < n)
                {
                    copy_map[pc->input_inst.var_index] = -1;
                }
                break;
            
            case OUT:
            {
                int resolved = resolve(pc->output_inst.var_index);
                if (resolved != pc->output_inst.var_index)
                {
                    pc->output_inst.var_index = resolved;   
                    propogated++;
                }
                break;
            }
            case CJMP:
            {
                int op1 = resolve(pc->cjmp_inst.operand1_index);
                int op2 = resolve(pc->cjmp_inst.operand2_index);
                if (op1 != pc->cjmp_inst.operand1_index)
                {
                    pc->cjmp_inst.operand1_index = op1;
                    propogated++;
                }
                if (op2 != pc->cjmp_inst.operand2_index)
                {
                    pc->cjmp_inst.operand2_index = op2;
                    propogated++;
                }
                break;
            }
            case SCMP:
            {
                int op1 = resolve(pc->scmp_inst.operand1_index);
                int op2 = resolve(pc->scmp_inst.operand2_index);    
                if (op1 != pc->scmp_inst.operand1_index)
                {
                    pc->scmp_inst.operand1_index = op1;
                    propogated++;
                }
                if (op2 != pc->scmp_inst.operand2_index)
                {
                    pc->scmp_inst.operand2_index = op2;
                    propogated++;
                }
                break;
            }
            case ARRAY_READ:
            {
                int resolved = resolve(pc->array_inst.index_slot);
                if (resolved != pc->array_inst.index_slot)
                {
                    pc->array_inst.index_slot = resolved;
                    propogated++;
                }
                if (pc->array_inst.target_index < n)
                {
                    copy_map[pc->array_inst.target_index] = -1;
                }
                break;
            }
            case ARRAY_WRITE:
            {
                int idx = resolve(pc->array_inst.index_slot);
                int val = resolve(pc->array_inst.target_index);
                if (idx != pc->array_inst.index_slot)
                {
                    pc->array_inst.index_slot = idx;
                    propogated++;
                }
                if (val != pc->array_inst.target_index)
                {
                    pc->array_inst.target_index = val;
                    propogated++;
                }
                break;
            }
            case CALL:
            {
                for (int i = 0;i < pc->call_inst.num_params;i++)
                {
                    int resolved = resolve(pc->call_inst.arg_val_slots[i]);
                    if (resolved != pc->call_inst.arg_val_slots[i])
                    {
                        pc->call_inst.arg_val_slots[i] = resolved;
                        propogated++;
                    }
                    // param slots get new values - invalidate
                    int p = pc->call_inst.param_slots[i];
                    if (p < n) copy_map[p] = -1;
                }
                break;
            }
            case RET:
            {
                int resolved = resolve(pc->ret_inst.ret_val_index);
                if (resolved != pc->ret_inst.ret_val_index)
                {
                    pc->ret_inst.ret_val_index = resolved;
                    propogated++;
                }
                break;
            }
            default: break;
        }
        pc = pc->next;
    }
    return propogated;
}

int dead_code_eliminate(struct InstructionNode* program)
{
    int n = next_available;

    // Pass 1 - mark all slots that are read anywhere
    vector<bool> is_read(n, false);
    
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        switch (pc->type)
        {
            case ASSIGN:
            {
                if (pc->assign_inst.op == OPERATOR_NONE)
                {
                    int src = pc->assign_inst.operand1_index;
                    if (src < n)
                    {
                        is_read[src] = true;
                    }
                }
                else 
                {
                    int op1 = pc->assign_inst.operand1_index;
                    int op2 = pc->assign_inst.operand2_index;
                    if (op1 < n) is_read[op1] = true;
                    if (op2 < n) is_read[op2] = true;
                }
                break;
            }
            case OUT:
            {
                if (pc->output_inst.var_index < n)
                {
                    is_read[pc->output_inst.var_index] = true;
                }
                break;
            }
            case CJMP:
            {
                if (pc->cjmp_inst.operand1_index < n) is_read[pc->cjmp_inst.operand1_index] = true;
                if (pc->cjmp_inst.operand2_index < n) is_read[pc->cjmp_inst.operand2_index] = true;
                break;
            }
            case SCMP:
            {
                if (pc->scmp_inst.operand1_index < n) is_read[pc->scmp_inst.operand1_index] = true;
                if (pc->scmp_inst.operand1_index < n) is_read[pc->scmp_inst.operand1_index] = true;
                break;
            }
            case ARRAY_READ:
            {
                if (pc->array_inst.index_slot < n) is_read[pc->array_inst.index_slot] = true;
                if (pc->array_inst.base_index < n) is_read[pc->array_inst.base_index] = true;
                if (pc->array_inst.size_slot >= 0 && pc->array_inst.size_slot < n)
                {
                    is_read[pc->array_inst.size_slot] = true;
                }
                break;
            }
            case ARRAY_WRITE:
            {
                if (pc->array_inst.index_slot < n) is_read[pc->array_inst.index_slot] = true;
                if (pc->array_inst.target_index < n) is_read[pc->array_inst.target_index] = true;
                if (pc->array_inst.base_index < n) is_read[pc->array_inst.base_index] = true;
                if (pc->array_inst.size_slot >= 0 && pc->array_inst.size_slot < n)
                {
                    is_read[pc->array_inst.size_slot] = true;
                }
                break;
            }
            case CALL:
            {
                for (int i = 0;i < pc->call_inst.num_params;i++)
                {
                    if (pc->call_inst.arg_val_slots[i] < n)
                    {
                        is_read[pc->call_inst.arg_val_slots[i]] = true;
                    }
                }
                if (pc->call_inst.ret_val_index < n)
                {
                    is_read[pc->call_inst.ret_val_index] = true;
                }
                break;  
            }
            case RET:
            {
                if (pc->ret_inst.ret_val_index < n)
                {
                    is_read[pc->ret_inst.ret_val_index] = true;
                }
                break;
            }
            case ALLOC:
            {
                if (pc->alloc_inst.size_slot < n) is_read[pc->alloc_inst.size_slot] = true;
                if (pc->alloc_inst.base_slot < n) is_read[pc->alloc_inst.base_slot] = true;
                break;
            }
            case STRCAT:
            {
                if (pc->strcat_inst.left_slot < n) is_read[pc->strcat_inst.left_slot] = true;
                if (pc->strcat_inst.right_slot < n) is_read[pc->strcat_inst.right_slot] = true;
                break;
            }
            default: break;
        }
        pc = pc->next;
    }

    // Pass 2 - eliminate OPERATOR_NONE assignments to unread slots
    int eliminated = 0;
    pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op == OPERATOR_NONE)
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
        bool eligible = !isRecursive.count(funcHead) && !hasControlFlow && callSiteCount[funcHead] <= MAX_CALL_SITES && bodySize[funcHead] <= MAX_BODY_NODES;

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

int count_ir_nodes(struct InstructionNode* program)
{
    int count = 0;
    struct InstructionNode* pc = program;
    while (pc != nullptr) 
    { 
        if (pc->type != NOOP)
            count++; 
        pc = pc->next; 
    }
    return count;
}

void dump_ir(struct InstructionNode* program)
{
    printf("\n");
    printf("===================================================\n");
    printf("  NovaComp IR Dump\n");
    printf("===================================================\n");

    struct InstructionNode* pc = program;
    int nodeIndex = 0;

    while (pc != NULL)
    {
        if (pc->type == NOOP)
        {
            pc = pc->next;
            continue;
        }
        printf("  [%3d]  ", nodeIndex++);
        switch (pc->type)
        {
            case NOOP: printf("NOOP\n"); break;
            case IN:   printf("IN          -> mem[%d]\n", pc->input_inst.var_index); break;
            case OUT:
                if (pc->output_inst.is_string)
                    printf("OUT         strMem[%d] \"%s\"%s\n",
                        pc->output_inst.var_index,
                        strMem[pc->output_inst.var_index].c_str(),
                        pc->output_inst.newline ? "  (newline)" : "");
                else
                    printf("OUT         mem[%d]%s\n",
                        pc->output_inst.var_index,
                        pc->output_inst.newline ? "  (newline)" : "");
                break;
            case ASSIGN:
                if (pc->assign_inst.op == OPERATOR_NONE)
                    printf("ASSIGN      mem[%d] <- mem[%d]\n",
                        pc->assign_inst.left_hand_side_index,
                        pc->assign_inst.operand1_index);
                else
                {
                    const char* opStr =
                        pc->assign_inst.op == OPERATOR_PLUS  ? "+" :
                        pc->assign_inst.op == OPERATOR_MINUS ? "-" :
                        pc->assign_inst.op == OPERATOR_MULT  ? "*" : "/";
                    printf("ASSIGN      mem[%d] <- mem[%d] %s mem[%d]\n",
                        pc->assign_inst.left_hand_side_index,
                        pc->assign_inst.operand1_index, opStr,
                        pc->assign_inst.operand2_index);
                }
                break;
            case CJMP:
            {
                const char* condStr =
                    pc->cjmp_inst.condition_op == CONDITION_GREATER ? ">"  :
                    pc->cjmp_inst.condition_op == CONDITION_LESS    ? "<"  : "<>";
                printf("CJMP        if mem[%d] %s mem[%d]  pass->next  fail->node@%p\n",
                    pc->cjmp_inst.operand1_index, condStr,
                    pc->cjmp_inst.operand2_index, (void*)pc->cjmp_inst.target);
                break;
            }
            case JMP:  printf("JMP         -> node@%p\n", (void*)pc->jmp_inst.target); break;
            case CALL:
                printf("CALL        func@%p  ret->mem[%d]  params:%d\n",
                    (void*)pc->call_inst.function_head,
                    pc->call_inst.ret_val_index, pc->call_inst.num_params);
                break;
            case RET:  printf("RET         mem[%d]\n", pc->ret_inst.ret_val_index); break;
            case ARRAY_READ:
                printf("ARRAY_READ  mem[%d + mem[%d]] -> mem[%d]\n",
                    pc->array_inst.base_index, pc->array_inst.index_slot,
                    pc->array_inst.target_index);
                break;
            case ARRAY_WRITE:
                printf("ARRAY_WRITE mem[%d] -> mem[%d + mem[%d]]\n",
                    pc->array_inst.target_index, pc->array_inst.base_index,
                    pc->array_inst.index_slot);
                break;
            case ALLOC:
                printf("ALLOC       base->mem[%d]  size=mem[%d]\n",
                    pc->alloc_inst.base_slot, pc->alloc_inst.size_slot);
                break;
            case STRCAT:
                printf("STRCAT      mem[%d] <- strMem[mem[%d]] + strMem[mem[%d]]\n",
                    pc->strcat_inst.dest_slot,
                    pc->strcat_inst.left_slot,
                    pc->strcat_inst.right_slot);
                break;
            case SCMP:
            {
                const char* condStr =
                    pc->scmp_inst.condition_op == CONDITION_GREATER ? ">"  :
                    pc->scmp_inst.condition_op == CONDITION_LESS    ? "<"  : "<>";
                printf("SCMP        if strMem[mem[%d]] %s strMem[mem[%d]]  pass->next  fail->node@%p\n",
                    pc->scmp_inst.operand1_index, condStr,
                    pc->scmp_inst.operand2_index, (void*)pc->scmp_inst.target);
                break;
            }
            default: printf("UNKNOWN     type=%d\n", pc->type); break;
        }
        pc = pc->next;
    }

    printf("===================================================\n");
    printf("  Total nodes: %d\n", nodeIndex);
    printf("===================================================\n\n");
}

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

// ── Graph Coloring Register Allocation ─────────────────────────────────────────────────────────

// ── Liveness Analysis ─────────────────────────────────────────────────────────

// Get all slots DEFINED (written) by an instruction
static void get_defs(struct InstructionNode* n, std::unordered_set<int>& defs)
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
            if (n->tensor_call_inst.result_slot >= 0)
            {
                defs.insert(n->tensor_call_inst.result_slot);
            }
            break;
        }
        default: break;
    }
}

// Get all slots USED (read) by an instruction
static void get_uses(struct InstructionNode* n, std::unordered_set<int>& uses)
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
            uses.insert(n->output_inst.var_index);
            break;
        }
        case CJMP:
        {
            uses.insert(n->cjmp_inst.operand1_index);
            uses.insert(n->cjmp_inst.operand2_index);
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
            for (int i = 0;i < n->call_inst.num_params;i++)
            {
                uses.insert(n->call_inst.arg_val_slots[i]);
            }
            break;
        }
        case RET:
        {
            uses.insert(n->ret_inst.ret_val_index);
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
            for (int i = 0;i < n->tensor_call_inst.num_args;i++)
            {
                uses.insert(n->tensor_call_inst.arg_slots[i]);
            }
            break;
        }
        default: break;
    }
}

// Get successor nodes of n (for CFG edges)
static void get_successors(struct InstructionNode* n, std::vector<struct InstructionNode*>& succs)
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
                           const unordered_set<int>& liveAcrossCallSlots = {},
                           const unordered_set<int>& constantSlots = {})
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
static unordered_set<int> find_constant_slots(struct InstructionNode* program)
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
                pc->call_inst.ret_val_index;
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

void generate_x86(struct InstructionNode* program, const std::string& outputFile,
                  const ColoringResult* coloring = nullptr)
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

    // ── Stack frame size (for main / spilled slots) ───────────────────────────
    int frameSize = (next_available + 1) * 8;
    if (frameSize % 16 != 0) frameSize += 16 - (frameSize % 16);

    // ── Compute per-function colorings ────────────────────────────────────────
    map<struct InstructionNode*, ColoringResult> funcColorings;
    for (auto& kv : funcLabelMap)
    {
        struct InstructionNode* fhead = kv.first;
        int fLabel = kv.second;  // ← get label here
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
    for (auto& kv : funcLabelMap)
    {
        struct InstructionNode* fhead = kv.first;
        if (funcColorings.count(fhead) && !funcColorings[fhead].spilled.empty())
        {
            // Re-run liveness after spill insertion
            fhead = insert_spill_code(fhead, funcColorings[fhead]);
            LivenessResult flr2 = compute_liveness(fhead);
            InterferenceGraph fig2 = build_interference_graph(fhead, flr2);
            unordered_set<int> lac2 = find_live_across_calls(fhead, flr2);
            unordered_set<int> const2 = find_constant_slots(fhead);
            funcColorings[fhead] = color_graph(fig2, lac2, const2);
            // Update funcLabelMap to point to new head
            funcLabelMap[fhead] = kv.second;
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

    for (auto& kv : funcLabelMap)
    {
        struct InstructionNode* head = kv.first;
        int funcLabel = kv.second;

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

// Main JIT compilation function
static void* jit_compile_function(struct InstructionNode* head)
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

void run_repl()
{
    printf("\n");
    printf("  NovaComp v2.0 REPL\n");
    printf("  Type statements one at a time.\n");
    printf("  Type 'exit' or 'quit' to quit.\n");
    printf("\n");
    fflush(stdout);

    string accumulated = "";  // full input collected so far
    int brace_depth = 0;  // tracks open {} to detect multi-line input
    bool in_def = false;  // true when inside a def block

    while (true)
    {
        // show prompt
        if (brace_depth == 0)
            printf(">>> ");
        else
            printf("... ");
        fflush(stdout);

        // read a line
        string line;
        if (!getline(cin, line))
            break;  // EOF

        // check for exit
        string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != string::npos) trimmed = trimmed.substr(start);
        // trim trailing whitespace
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != string::npos) trimmed = trimmed.substr(0, end + 1);


        if (trimmed == "exit" || trimmed == "quit")
        {
            printf("\n  Bye!\n\n");
            break;
        }

        // accumulated line
        accumulated += line + "\n";

        // count brace depth
        for (char c : line)
        {
            if (c == '{') brace_depth++;
            if (c == '}') brace_depth--;
        }

        // brace_depth should never go below 0
        if (brace_depth < 0) brace_depth = 0;

        // if braces are still open - keep reading
        if (brace_depth > 0)
        {
            continue;
        }

        // we have a complete input - parse and execute
        if (accumulated.find_first_not_of(" \t\r\n") == string::npos)
        {
            // blank input - reset and continue
            accumulated = "";
            continue;
        }

        // clear errors from previous iterations
        errorList.clear();

        // parse
        string wrapped = accumulated;
        bool hasTopLevel = (accumulated.find("main()") != string::npos ||
                           accumulated.find("def ") != string::npos ||
                           accumulated.find("class ") != string::npos ||
                           accumulated.find("struct ") != string::npos ||
                           accumulated.find("import ") != string::npos);
        if (!hasTopLevel) 
        {
            // Pure statement - wrap in main()
            wrapped = "main()\n{\n" + accumulated + "\n}\n";
        }
        else if (accumulated.find("main()") == string::npos)
        {
            // Top-level def/class/import without main() - append empty main()
            wrapped = accumulated + "\nmain()\n{\n}\n";
        }
        set<string> already_imported;
        string processed = preprocess_import(wrapped, ".", already_imported);
        lexer.ReinitializeFromString(processed);
        struct InstructionNode* program = parse_generate_intermediate_representation();

        if (!errorList.empty())
        {
            for (const string& err : errorList)
                fprintf(stderr, "%s\n", err.c_str());
            errorList.clear();
            accumulated = "";
            brace_depth = 0;
            continue; // recover insterad of crashing
        }
        else if (program != nullptr)
        {
            // execute
            input_replay_index = -1;  // live stdin mode
            execute_program(program);
        }

        // reset for next input
        accumulated = "";
        brace_depth = 0;
        in_def = false;
    }
}

// ── CUDA codegen ─────────────────────────────────────────────────────────────

void generate_cuda(struct InstructionNode* program, const std::string& outputFile)
{
    // Pass 1: collect which tensor op types are actually used in the IR
    std::unordered_set<int> ops_used;
    for (auto* p = program; p; p = p->next)
        if (p->type == TENSOR_CALL)
            ops_used.insert((int)p->tensor_call_inst.op);

    if (ops_used.empty())
    {
        fprintf(stderr, "Error: --emit-cuda requires tensor operations in source\n");
        return;
    }

    auto has = [&](TensorOp op) { return ops_used.count((int)op) > 0; };

    bool needMatmul   = has(TEN_MATMUL);
    bool needAdd      = has(TEN_ADD);
    bool needRelu     = has(TEN_RELU) || has(TEN_RELU_GRAD);
    bool needSoftmax  = has(TEN_SOFTMAX);
    bool needBackward = has(TEN_BACKWARD) || has(TEN_RELU_GRAD) || has(TEN_SIGMOID_GRAD)
                     || has(TEN_MSE_GRAD) || has(TEN_BCE_GRAD)  || has(TEN_CE_GRAD);

    FILE* out = fopen(outputFile.c_str(), "w");
    if (!out)
    {
        fprintf(stderr, "Error: could not open output file '%s'\n", outputFile.c_str());
        return;
    }

    // ── 1. Header ────────────────────────────────────────────────────────────
    fprintf(out,
        "// Generated by NovaComp --emit-cuda\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <cuda_runtime.h>\n"
        "#include <math.h>\n\n");

    // ── 2. CUDA_CHECK macro ───────────────────────────────────────────────────
    fprintf(out,
        "#define CUDA_CHECK(call) \\\n"
        "    do { \\\n"
        "        cudaError_t err = call; \\\n"
        "        if (err != cudaSuccess) { \\\n"
        "            fprintf(stderr, \"CUDA error at %%s:%%d: %%s\\n\", \\\n"
        "                    __FILE__, __LINE__, cudaGetErrorString(err)); \\\n"
        "            exit(1); \\\n"
        "        } \\\n"
        "    } while(0)\n\n");

    // ── 3. Kernels (only those actually present in IR) ────────────────────────
    if (needMatmul)
    {
        fprintf(out,
            "__global__ void matmul_kernel(float* A, float* B, float* C, int M, int K, int N) {\n"
            "    int row = blockIdx.y * blockDim.y + threadIdx.y;\n"
            "    int col = blockIdx.x * blockDim.x + threadIdx.x;\n"
            "    if (row < M && col < N) {\n"
            "        float sum = 0.0f;\n"
            "        for (int k = 0; k < K; k++)\n"
            "            sum += A[row * K + k] * B[k * N + col];\n"
            "        C[row * N + col] = sum;\n"
            "    }\n"
            "}\n\n");
    }

    if (needAdd)
    {
        fprintf(out,
            "__global__ void add_kernel(float* A, float* B, float* C, int n) {\n"
            "    int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
            "    if (i < n) C[i] = A[i] + B[i];\n"
            "}\n\n");
    }

    if (needRelu)
    {
        fprintf(out,
            "__global__ void relu_kernel(float* A, float* C, int n) {\n"
            "    int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
            "    if (i < n) C[i] = A[i] > 0.0f ? A[i] : 0.0f;\n"
            "}\n\n");
    }

    if (needSoftmax)
    {
        fprintf(out,
            "__global__ void softmax_kernel(float* A, float* C, int n) {\n"
            "    float sum = 0.0f;\n"
            "    for (int i = 0; i < n; i++) sum += expf(A[i]);\n"
            "    int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
            "    if (i < n) C[i] = expf(A[i]) / sum;\n"
            "}\n\n");
    }

    if (needBackward)
    {
        fprintf(out,
            "__global__ void backward_kernel(float* grad_out, float* activation, float* grad_in, int n) {\n"
            "    int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
            "    if (i < n) grad_in[i] = grad_out[i] * (activation[i] > 0.0f ? 1.0f : 0.0f);\n"
            "}\n\n");
    }

    // ── 4. main() ─────────────────────────────────────────────────────────────
    fprintf(out, "int main() {\n");
    fprintf(out, "    srand(42);\n\n");

    // Track tensor slots: result_slot -> {device ptr name, host ptr name, rows, cols}
    struct TenInfo { std::string dev, host; int rows, cols; };
    std::unordered_map<int, TenInfo> tenMap;   // slot -> info
    std::vector<std::pair<std::string,std::string>> allPtrs; // (dev, host) for cleanup
    int counter = 0;
    std::string lastDev, lastHost;
    int lastN = 0;

    auto newTensor = [&](int rows, int cols) -> TenInfo {
        std::string base = "t" + std::to_string(counter++);
        return { "d_" + base, "h_" + base, rows, cols };
    };

    auto emitAlloc = [&](const TenInfo& ti, bool randomInit) {
        int n = ti.rows * ti.cols;
        fprintf(out, "    float *%s = (float*)malloc(%d * sizeof(float));\n", ti.host.c_str(), n);
        fprintf(out, "    float *%s;\n", ti.dev.c_str());
        fprintf(out, "    CUDA_CHECK(cudaMalloc(&%s, %d * sizeof(float)));\n", ti.dev.c_str(), n);
        if (randomInit)
            fprintf(out, "    for (int i = 0; i < %d; i++) %s[i] = (float)rand() / RAND_MAX;\n", n, ti.host.c_str());
        else
            fprintf(out, "    for (int i = 0; i < %d; i++) %s[i] = 0.0f;\n", n, ti.host.c_str());
        fprintf(out, "    CUDA_CHECK(cudaMemcpy(%s, %s, %d * sizeof(float), cudaMemcpyHostToDevice));\n\n",
                ti.dev.c_str(), ti.host.c_str(), n);
        allPtrs.push_back({ti.dev, ti.host});
        lastDev = ti.dev; lastHost = ti.host; lastN = n;
    };

    // Walk IR: emit CUDA code for each tensor operation in source order
    for (auto* p = program; p; p = p->next)
    {
        // Propagate tensor slot copies through integer ASSIGN instructions.
        // OPERATOR_NONE is a direct copy (mem[lhs] = mem[op1]), op2 is unused.
        // For arithmetic forms, only propagate when exactly one operand is a tensor slot.
        if (p->type == ASSIGN)
        {
            int lhs = p->assign_inst.left_hand_side_index;
            int op1 = p->assign_inst.operand1_index;
            if (p->assign_inst.op == OPERATOR_NONE)
            {
                if (tenMap.count(op1))
                    tenMap[lhs] = tenMap[op1];
            }
            else
            {
                int op2 = p->assign_inst.operand2_index;
                if (tenMap.count(op1) && !tenMap.count(op2))
                    tenMap[lhs] = tenMap[op1];
                else if (tenMap.count(op2) && !tenMap.count(op1))
                    tenMap[lhs] = tenMap[op2];
            }
            continue;
        }

        if (p->type != TENSOR_CALL) continue;

        TensorOp op   = p->tensor_call_inst.op;
        int      rslot = p->tensor_call_inst.result_slot;

        // ── Allocation ops ───────────────────────────────────────────────────
        if (op == TEN_ALLOC || op == TEN_ZEROS || op == TEN_ONES
         || op == TEN_RANDN || op == TEN_XAVIER)
        {
            int rows = 32, cols = 32;  // default; extract from IR when possible
            TenInfo ti = newTensor(rows, cols);
            fprintf(out, "    /* tensor %s (%dx%d) */\n",
                    ti.dev.c_str(), rows, cols);
            bool rnd = (op == TEN_ALLOC || op == TEN_RANDN || op == TEN_XAVIER);
            emitAlloc(ti, rnd);
            tenMap[rslot] = ti;
            continue;
        }

        // ── matmul ───────────────────────────────────────────────────────────
        if (op == TEN_MATMUL)
        {
            int sa = p->tensor_call_inst.arg_slots[0];
            int sb = p->tensor_call_inst.arg_slots[1];
            if (!tenMap.count(sa) || !tenMap.count(sb))
            {
                fprintf(out, "    /* skipped matmul: tensor args not tracked */\n\n");
                continue;
            }
            auto& A = tenMap[sa];
            auto& B = tenMap[sb];
            int K = A.cols;
            TenInfo ti = newTensor(A.rows, B.cols);
            int n = ti.rows * ti.cols;
            fprintf(out, "    /* matmul: %s = %s x %s (%dx%d) */\n",
                    ti.dev.c_str(), A.dev.c_str(), B.dev.c_str(), ti.rows, ti.cols);
            fprintf(out, "    float *%s = (float*)malloc(%d * sizeof(float));\n", ti.host.c_str(), n);
            fprintf(out, "    float *%s;\n", ti.dev.c_str());
            fprintf(out, "    CUDA_CHECK(cudaMalloc(&%s, %d * sizeof(float)));\n", ti.dev.c_str(), n);
            fprintf(out, "    {\n");
            fprintf(out, "        dim3 block(16, 16);\n");
            fprintf(out, "        dim3 grid((%d + 15) / 16, (%d + 15) / 16);\n", ti.cols, ti.rows);
            fprintf(out, "        matmul_kernel<<<grid, block>>>(%s, %s, %s, %d, %d, %d);\n",
                    A.dev.c_str(), B.dev.c_str(), ti.dev.c_str(), ti.rows, K, ti.cols);
            fprintf(out, "        CUDA_CHECK(cudaGetLastError());\n");
            fprintf(out, "        CUDA_CHECK(cudaDeviceSynchronize());\n");
            fprintf(out, "    }\n\n");
            allPtrs.push_back({ti.dev, ti.host});
            lastDev = ti.dev; lastHost = ti.host; lastN = n;
            tenMap[rslot] = ti;
            continue;
        }

        // ── element-wise add ─────────────────────────────────────────────────
        if (op == TEN_ADD)
        {
            int sa = p->tensor_call_inst.arg_slots[0];
            int sb = p->tensor_call_inst.arg_slots[1];
            if (!tenMap.count(sa) || !tenMap.count(sb))
            {
                fprintf(out, "    /* skipped add: tensor args not tracked */\n\n");
                continue;
            }
            auto& A = tenMap[sa];
            TenInfo ti = newTensor(A.rows, A.cols);
            int n = ti.rows * ti.cols;
            fprintf(out, "    /* add: %s = %s + %s */\n",
                    ti.dev.c_str(), A.dev.c_str(), tenMap[sb].dev.c_str());
            fprintf(out, "    float *%s = (float*)malloc(%d * sizeof(float));\n", ti.host.c_str(), n);
            fprintf(out, "    float *%s;\n", ti.dev.c_str());
            fprintf(out, "    CUDA_CHECK(cudaMalloc(&%s, %d * sizeof(float)));\n", ti.dev.c_str(), n);
            fprintf(out, "    {\n");
            fprintf(out, "        int threads = 256;\n");
            fprintf(out, "        int blocks = (%d + threads - 1) / threads;\n", n);
            fprintf(out, "        add_kernel<<<blocks, threads>>>(%s, %s, %s, %d);\n",
                    A.dev.c_str(), tenMap[sb].dev.c_str(), ti.dev.c_str(), n);
            fprintf(out, "        CUDA_CHECK(cudaGetLastError());\n");
            fprintf(out, "        CUDA_CHECK(cudaDeviceSynchronize());\n");
            fprintf(out, "    }\n\n");
            allPtrs.push_back({ti.dev, ti.host});
            lastDev = ti.dev; lastHost = ti.host; lastN = n;
            tenMap[rslot] = ti;
            continue;
        }

        // ── relu / relu_grad ─────────────────────────────────────────────────
        if (op == TEN_RELU || op == TEN_RELU_GRAD)
        {
            int sa = p->tensor_call_inst.arg_slots[0];
            if (!tenMap.count(sa))
            {
                fprintf(out, "    /* skipped relu: tensor arg not tracked */\n\n");
                continue;
            }
            auto& A = tenMap[sa];
            TenInfo ti = newTensor(A.rows, A.cols);
            int n = ti.rows * ti.cols;
            const char* opname = (op == TEN_RELU) ? "relu" : "relu_grad";
            fprintf(out, "    /* %s: %s = %s(%s) */\n",
                    opname, ti.dev.c_str(), opname, A.dev.c_str());
            fprintf(out, "    float *%s = (float*)malloc(%d * sizeof(float));\n", ti.host.c_str(), n);
            fprintf(out, "    float *%s;\n", ti.dev.c_str());
            fprintf(out, "    CUDA_CHECK(cudaMalloc(&%s, %d * sizeof(float)));\n", ti.dev.c_str(), n);
            fprintf(out, "    {\n");
            fprintf(out, "        int threads = 256;\n");
            fprintf(out, "        int blocks = (%d + threads - 1) / threads;\n", n);
            fprintf(out, "        relu_kernel<<<blocks, threads>>>(%s, %s, %d);\n",
                    A.dev.c_str(), ti.dev.c_str(), n);
            fprintf(out, "        CUDA_CHECK(cudaGetLastError());\n");
            fprintf(out, "        CUDA_CHECK(cudaDeviceSynchronize());\n");
            fprintf(out, "    }\n\n");
            allPtrs.push_back({ti.dev, ti.host});
            lastDev = ti.dev; lastHost = ti.host; lastN = n;
            tenMap[rslot] = ti;
            continue;
        }

        // ── softmax ──────────────────────────────────────────────────────────
        if (op == TEN_SOFTMAX)
        {
            int sa = p->tensor_call_inst.arg_slots[0];
            if (!tenMap.count(sa))
            {
                fprintf(out, "    /* skipped softmax: tensor arg not tracked */\n\n");
                continue;
            }
            auto& A = tenMap[sa];
            TenInfo ti = newTensor(A.rows, A.cols);
            int n = ti.rows * ti.cols;
            fprintf(out, "    /* softmax: %s = softmax(%s) */\n",
                    ti.dev.c_str(), A.dev.c_str());
            fprintf(out, "    float *%s = (float*)malloc(%d * sizeof(float));\n", ti.host.c_str(), n);
            fprintf(out, "    float *%s;\n", ti.dev.c_str());
            fprintf(out, "    CUDA_CHECK(cudaMalloc(&%s, %d * sizeof(float)));\n", ti.dev.c_str(), n);
            fprintf(out, "    {\n");
            fprintf(out, "        int threads = 256;\n");
            fprintf(out, "        int blocks = (%d + threads - 1) / threads;\n", n);
            fprintf(out, "        softmax_kernel<<<blocks, threads>>>(%s, %s, %d);\n",
                    A.dev.c_str(), ti.dev.c_str(), n);
            fprintf(out, "        CUDA_CHECK(cudaGetLastError());\n");
            fprintf(out, "        CUDA_CHECK(cudaDeviceSynchronize());\n");
            fprintf(out, "    }\n\n");
            allPtrs.push_back({ti.dev, ti.host});
            lastDev = ti.dev; lastHost = ti.host; lastN = n;
            tenMap[rslot] = ti;
            continue;
        }

        // ── backward / gradient ops → backward_kernel ────────────────────────
        if (op == TEN_MSE_GRAD || op == TEN_BCE_GRAD || op == TEN_CE_GRAD
         || op == TEN_SIGMOID_GRAD)
        {
            int sa = p->tensor_call_inst.arg_slots[0];
            int sb = p->tensor_call_inst.arg_slots[1];
            if (!tenMap.count(sa) || !tenMap.count(sb))
            {
                fprintf(out, "    /* skipped backward: tensor args not tracked */\n\n");
                continue;
            }
            auto& A = tenMap[sa];
            auto& B = tenMap[sb];
            TenInfo ti = newTensor(A.rows, A.cols);
            int n = ti.rows * ti.cols;
            fprintf(out, "    /* backward: %s = backward(%s, %s) */\n",
                    ti.dev.c_str(), A.dev.c_str(), B.dev.c_str());
            fprintf(out, "    float *%s = (float*)malloc(%d * sizeof(float));\n", ti.host.c_str(), n);
            fprintf(out, "    float *%s;\n", ti.dev.c_str());
            fprintf(out, "    CUDA_CHECK(cudaMalloc(&%s, %d * sizeof(float)));\n", ti.dev.c_str(), n);
            fprintf(out, "    {\n");
            fprintf(out, "        int threads = 256;\n");
            fprintf(out, "        int blocks = (%d + threads - 1) / threads;\n", n);
            fprintf(out, "        backward_kernel<<<blocks, threads>>>(%s, %s, %s, %d);\n",
                    A.dev.c_str(), B.dev.c_str(), ti.dev.c_str(), n);
            fprintf(out, "        CUDA_CHECK(cudaGetLastError());\n");
            fprintf(out, "        CUDA_CHECK(cudaDeviceSynchronize());\n");
            fprintf(out, "    }\n\n");
            allPtrs.push_back({ti.dev, ti.host});
            lastDev = ti.dev; lastHost = ti.host; lastN = n;
            tenMap[rslot] = ti;
            continue;
        }
    }

    // ── Copy last result back to host and print ───────────────────────────────
    if (!lastDev.empty())
    {
        fprintf(out, "    /* copy result back */\n");
        fprintf(out, "    CUDA_CHECK(cudaMemcpy(%s, %s, %d * sizeof(float), cudaMemcpyDeviceToHost));\n",
                lastHost.c_str(), lastDev.c_str(), lastN);
        fprintf(out, "    printf(\"Result[0..4]: \");\n");
        fprintf(out, "    for (int i = 0; i < 5 && i < %d; i++) printf(\"%%.4f \", %s[i]);\n",
                lastN, lastHost.c_str());
        fprintf(out, "    printf(\"\\n\");\n\n");
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (auto& pr : allPtrs)
    {
        fprintf(out, "    cudaFree(%s);\n", pr.first.c_str());
        fprintf(out, "    free(%s);\n", pr.second.c_str());
    }
    fprintf(out, "\n    return 0;\n}\n");

    fclose(out);
    fprintf(stderr, "CUDA output written to: %s\n", outputFile.c_str());
}

// main

#ifndef NOVA_NOVM_BUILD
int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);  // disable stdout buffering
    setvbuf(stderr, nullptr, _IONBF, 0); // disable stderr buffering

    string inputFile       = "";
    bool flag_dump_ir      = false;
    bool flag_optimize     = false;
    bool flag_benchmark    = false;
    bool flag_repl         = false;
    int  bench_iters       = 10000;
    bool flag_emit_asm     = false;
    bool flag_build        = false;
    bool flag_emit_cuda    = false;
    bool flag_emit_bytecode= false;
    string asmFile         = "";
    string cudaOutputFile  = "";
    string bytecodeOutFile = "";

    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--dump-ir")        { flag_dump_ir        = true; continue; }
        if (arg == "--optimize")       { flag_optimize       = true; continue; }
        if (arg == "--benchmark")      { flag_benchmark      = true; continue; }
        if (arg == "--emit-asm")       { flag_emit_asm       = true; continue; }
        if (arg == "--build")          { flag_build          = true; continue; }
        if (arg == "--repl")           { flag_repl           = true; continue; }
        if (arg == "--debug")          { debug_mode = true; debug_step = true; continue; }
        if (arg == "--emit-cuda")      { flag_emit_cuda      = true; continue; }
        if (arg == "--emit-bytecode")  { flag_emit_bytecode  = true; continue; }
        if (arg[0] != '-')             { inputFile = arg; continue; }

        if (arg == "--iters" && i + 1 < argc)
        {
            bench_iters = atoi(argv[++i]);
            continue;
        }
        if (arg == "-o" && i + 1 < argc)
        {
            bytecodeOutFile = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown flag: %s\n", arg.c_str());
        fprintf(stderr, "Usage: %s [file.csl] [--dump-ir] [--optimize] [--benchmark] [--iters N] [--emit-cuda] [--emit-bytecode] [-o out.nbc]\n",
                argv[0]);
        return 1;
    }

    // REPL mode 
    if (flag_repl)
    {
        run_repl();
        return 0;
    }

    string processed = "";

    // file mode 
    if (!inputFile.empty())
    {
        // Accept both .csl and .nova extensions
        bool validExt = false;
        if (inputFile.size() >= 5 && inputFile.substr(inputFile.size() - 4) == ".csl") validExt = true;
        if (inputFile.size() >= 6 && inputFile.substr(inputFile.size() - 5) == ".nova") validExt = true;
        if (!validExt)
        {
            fprintf(stderr, "Error: file must have a .csl or .nova extension\n");
            return 1;
        }
        FILE* test = fopen(inputFile.c_str(), "r");
        if (!test)
        {
            fprintf(stderr, "Error: file '%s' not found\n", inputFile.c_str());
            return 1;
        }
        fclose(test);

        string raw_source = read_file(inputFile);
        string base_dir = inputFile;
        size_t last_slash = base_dir.find_last_of("/\\");
        base_dir = (last_slash != string::npos) ? base_dir.substr(0, last_slash) : ".";
        set<string> already_imported;
        processed = preprocess_import(raw_source, base_dir, already_imported);
        lexer.ReinitializeFromString(processed);
    }
    else
    {
        // stdin mode (./compiler < program.csl)
        ostringstream raw_stream;
        raw_stream << cin.rdbuf();
        string raw_source = raw_stream.str();
        set<string> already_imported;
        processed = preprocess_import(raw_source, ".", already_imported);
        lexer.ReinitializeFromString(processed);
    }

    //Calculate how many lines were added by imports
    if (debug_mode)
    {
        int processed_lines = (int)std::count(processed.begin(), processed.end(), '\n');
        int raw_lines = 0;
        if (!inputFile.empty())
        {
            string raw = read_file(inputFile);
            raw_lines = (int)std::count(raw.begin(), raw.end(), '\n');
        }
        import_line_offset = std::max(0, processed_lines - raw_lines);
    }

    // parse 
    struct InstructionNode* program = parse_generate_intermediate_representation();

    if (flag_dump_ir)
    {
        LivenessResult lr = compute_liveness(program);
        dump_liveness(program, lr);
        InterferenceGraph ig = build_interference_graph(program, lr);
        dump_interference_graph(ig);
        ColoringResult cr = color_graph(ig);
        dump_coloring(cr);

        // Also test on fib function body if it exists
        // Liveness + interference for FIB function
        if (functionTable.count("fib"))
        {
            LivenessResult lr2 = compute_liveness(functionTable["fib"]);
            dump_liveness(functionTable["fib"], lr2);
            InterferenceGraph ig2 = build_interference_graph(functionTable["fib"], lr2);
            dump_interference_graph(ig2);
            ColoringResult cr2 = color_graph(ig2);
            dump_coloring(cr2);

            // Dump fib IR
            printf("\n-- fib IR \n");
            dump_ir(functionTable["fib"]);
        }
    }

    // // Temporary debug — check if symbolTable has anything
    // fprintf(stderr, "DEBUG symbolTable size: %d\n", (int)symbolTable.size());
    // for (auto& kv : symbolTable)
    //     fprintf(stderr, "  %s -> slot %d\n", kv.first.c_str(), kv.second);

    if (debug_mode)
    {
        debug_symbol_snapshot.clear();
        for (auto& kv : symbolTable)
        {
            debug_symbol_snapshot[kv.first] = {kv.second, TYPE_INT};
        }
        for (auto& kv : floatSymbolTable)
        {
            debug_symbol_snapshot[kv.first] = {kv.second, TYPE_FLOAT};
        }
        for (auto& kv : doubleSymbolTable)
        {
            debug_symbol_snapshot[kv.first] = {kv.second, TYPE_DOUBLE};
        }
    }

    // optimize 
    if (flag_optimize)
    {
        int folds   = constant_fold(program);
        int removed = remove_self_copies(program);
        int simplified = algaebric_simplify(program);
        removed += remove_self_copies(program);
        int propogated = copy_propogate(program);
        removed += remove_self_copies(program);
        int eliminated = dead_code_eliminate(program);
        removed += remove_self_copies(program);
        int hoisted = loop_invariant_code_motion(program);
        int inlined = inline_functions(program);
        // removed += remove_self_copies(program);
        (void)folds; (void)removed; (void)simplified; (void)propogated;
        (void)eliminated; (void)hoisted; (void)inlined;
    }

    // Identify pure + recursive functions for automatic memoization
    detect_memoizable_functions();

    // CUDA codegen — output .cu file and exit (does not execute the program)
    if (flag_emit_cuda)
    {
        if (inputFile.empty())
        {
            fprintf(stderr, "Error: --emit-cuda requires an input file\n");
            return 1;
        }
        cudaOutputFile = inputFile;
        size_t dot = cudaOutputFile.rfind('.');
        if (dot != string::npos) cudaOutputFile = cudaOutputFile.substr(0, dot);
        cudaOutputFile += ".cu";
        generate_cuda(program, cudaOutputFile);
        return 0;
    }

    // Bytecode emit — serialize IR to .nbc and exit
    if (flag_emit_bytecode)
    {
        if (bytecodeOutFile.empty())
        {
            if (inputFile.empty())
            {
                fprintf(stderr, "Error: --emit-bytecode requires an input file or -o <out.nbc>\n");
                return 1;
            }
            bytecodeOutFile = inputFile;
            size_t dot = bytecodeOutFile.rfind('.');
            if (dot != string::npos) bytecodeOutFile = bytecodeOutFile.substr(0, dot);
            bytecodeOutFile += ".nbc";
        }
        emit_bytecode(program, bytecodeOutFile);
        return 0;
    }

    if (flag_emit_asm)
    {
        // derive output Filename from input (program.csl -> program.asm)
        asmFile = inputFile;
        size_t dot = asmFile.rfind('.');
        if (dot != string::npos) asmFile = asmFile.substr(0, dot);
        asmFile += ".asm";

        // Running register allocation for main and all functions
        LivenessResult lr_main = compute_liveness(program);
        InterferenceGraph ig_main = build_interference_graph(program, lr_main);
        ColoringResult cr_main = color_graph(ig_main);

        // use only main coloring
        generate_x86(program, asmFile, &cr_main);

        if (flag_build)
        {
            // nasm -> object file
            string objFile = asmFile.substr(0, asmFile.rfind('.')) + ".o";
            string binFile = asmFile.substr(0, asmFile.rfind('.'));

            string nasmCmd = "nasm -f elf64 " + asmFile + " -o " + objFile;
            string linkCmd = "gcc -o " + binFile + " " + objFile + " -no-pie";

            fprintf(stderr, "Assembling: %s\n", nasmCmd.c_str());
            int r1 = system(nasmCmd.c_str());
            if (r1 != 0) 
            {
                fprintf(stderr, "nasm failed\n");
                return 1;
            }

            fprintf(stderr, "Linking:    %s\n", linkCmd.c_str());
            int r2 = system(linkCmd.c_str());
            if (r2 != 0)
            {
                fprintf(stderr, "gcc link failed\n");
                return 1;
            }

            fprintf(stderr, "Binary:     %s\n", binFile.c_str());   
        }
    }

    // IR dump 
    if (flag_dump_ir)
        dump_ir(program);

    // benchmark mode 
    if (flag_benchmark)
    {
        int nodeCount = count_ir_nodes(program);

        // warm-up run — live stdin, captures inputs into input_replay_buffer
        suppress_output = true;
        input_replay_index = -1;        // live stdin mode — fills replay buffer
        input_replay_buffer.clear();
        execute_program(program);

        // switch to replay mode — reuses captured inputs for all timed runs
        input_replay_index = 0;

        auto start = chrono::high_resolution_clock::now();
        for (int i = 0; i < bench_iters; i++)
        {
            input_replay_index = 0;     // rewind replay buffer each iteration
            execute_program(program);
        }
        auto end = chrono::high_resolution_clock::now();
        suppress_output = false;

        double total_us = chrono::duration<double, micro>(end - start).count();
        double avg_us   = total_us / bench_iters;

        printf("BENCH_US:%.3f\n",  avg_us);
        printf("BENCH_NODES:%d\n", nodeCount);
        printf("BENCH_ITERS:%d\n", bench_iters);
    }
    // normal single execution
    input_replay_index = -1;  // live stdin mode

#ifdef _WIN32
    // Pre-compute colorings for all functions so JIT can reuse them
    for (auto& kv : functionTable)
    {
        struct InstructionNode* fhead = kv.second;
        LivenessResult flr = compute_liveness(fhead);
        InterferenceGraph fig = build_interference_graph(fhead, flr);
        unordered_set<int> lacSlots = find_live_across_calls(fhead, flr);
        unordered_set<int> constSlots = find_constant_slots(fhead);
        g_funcColorings[fhead] = color_graph(fig, lacSlots, constSlots);
    }

    // Build param count map by scanning all call sites in main + every function
    unordered_map<InstructionNode*, int> funcParamCount;
    auto scanForCalls = [&](struct InstructionNode* ir) {
        for (struct InstructionNode* p = ir; p != nullptr; p = p->next)
            if (p->type == CALL)
                funcParamCount[p->call_inst.function_head] = p->call_inst.num_params;
    };
    scanForCalls(program);
    for (auto& kv2 : functionTable) scanForCalls(kv2.second);

    // Pre-JIT compile all functions before execution
    for (auto& kv : functionTable)
    {
        struct InstructionNode* fhead = kv.second;
        // JIT only handles single-parameter functions for now
        if (funcParamCount.count(fhead) && funcParamCount[fhead] > 1) continue;
        void* native = jit_compile_function(fhead);
        if (native) jit_compiled[fhead] = native;
    }
#endif

    // Temp Test: corrupt one compiled function to force deopt
    for (auto& kv : jit_compiled)
    {
        kv.second = (void*)0xDEADBEEF;  // bad pointer -> SEH will catch it 
        break;
    }

    execute_program(program);
    fflush(stdout);

    return 0;
}
#endif  // NOVA_NOVM_BUILD