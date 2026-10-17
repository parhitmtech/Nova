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
#include "progress_bar.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#ifndef _WIN32
#include <unistd.h>  // sleep() while polling SOL jobs
#endif

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
    VarType dest_type = TYPE_UNKNOWN;
    int slot_base;
    vector<int> savedSlots;
    vector<int> allRetSlots;
    vector<VarType> allRetTypes;
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

void tensor_set_shape(Tensor& t, const std::vector<int>& shape)
{
    t.shape = shape;
    int n = (int)shape.size();
    t.strides.assign(n, 1);
    for (int i = n - 2; i >= 0; i--) t.strides[i] = t.strides[i+1] * shape[i + 1];
    //2-D view: last dimension is cols, everything before it folds into rows
    t.cols = (n == 0) ? 1 : shape[n - 1];
    t.rows = 1;
    for (int i = 0; i < n - 1; i++) t.rows *= shape[i];
}

int alloc_tensor_nd(const std::vector<int>& shape)
{
    Tensor t;
    tensor_set_shape(t, shape);
    size_t numel = 1;
    for (int d : shape) numel *= (size_t)d;
    t.data.assign(numel, 0.0);
    t.refCount = 1;  // owned by whichever mem[] slot recieves this handle
    t.freed = false;
    tensor_heap.push_back(std::move(t));
    return (int)tensor_heap.size() - 1;
}

int alloc_tensor(int rows, int cols)
{
    return alloc_tensor_nd({rows, cols});
}

int alloc_tensor_like(int src)
{
    // copy the shape first: allocating can move tensor_heap
    std::vector<int> shape = tensor_heap[src].shape;
    return alloc_tensor_nd(shape);
}

// Runtime shape errors stop the program with the line number, like the freed-handle errors do.
[[noreturn]] static void tensor_shape_error(int line, const std::string& msg)
{
    fprintf(stderr, "Runtime error at line %d: %s\n", line, msg.c_str());
    exit(1);
}

// Flat offset of an N-D index, with rank and bounds checks.
static size_t tensor_offset(const Tensor& T, const int* idx, int n, const char* op, int line)
{
    if (n != T.ndim())
    {
        tensor_shape_error(line, std::string(op) + ": a rank-" + std::to_string(T.ndim()) + 
                           " tensor needs " + std::to_string(T.ndim()) + " indices, got " + std::to_string(n));
        size_t off = 0;
        for (int k = 0; k < n; k++)
        {
            if (idx[k] < 0 || idx[k] >= T.shape[k])
            {
                tensor_shape_error(line, std::string(op) + ": index " + std::to_string(idx[k]) + 
                                   " out of range for dimension " + std::to_string(k) + 
                                   " (size " + std::to_string(T.shape[k]) + ")");
            }
            off += (size_t)idx[k] * T.strides[k];
        }
        return off;
    }
}

// Ops that still work on the 2_D view only refuse N-D input instead of silently treating it as a 
// flattened matrix.
static void require_2d(int h, const char* op, int line)
{
    if (tensor_heap[h].ndim() > 2)
    {
        tensor_shape_error(line, std::string(op) + " supports 1-D and 2-D tensors only (got rank " + 
                           std::to_string(tensor_heap[h].ndim()) + "); use tensor_reshape first");
    }
}

static int ten_reshape(int a, const std::vector<int>& shape)
{
    int h = alloc_tensor_nd(shape);
    tensor_heap[h].data = tensor_heap[a].data;  // same elements, same order
    return h;
}

bool tensor_is_live(int h)
{
    return h >= 0 && h < (int)tensor_heap.size() && !tensor_heap[h].freed;
}

// A second mem[] slot now also aliases this handle (plain reassignment, or a tensor passed into a function
// call as an argument). Must be paired with an eventual tensor_release() for that same new alias, or the tensor leaks.
void tensor_retain(int h)
{
    if (!tensor_is_live(h)) return;
    tensor_heap[h].refCount++;
}

// One fewer mem[] slot holds this handle (compiler-inserted, at the slot's last use per liveness 
// analysis). At refcoutn 0, the backing data is freed immediately; the tensor_heap entry itself is
// kept (index stability) but marked freed so any further access is a loud, detectable error instead
// of silent corruption.
void tensor_release(int h)
{
    if (!tensor_is_live(h)) return; // already freed, or not a tracked handle
    tensor_heap[h].refCount--;
    if (tensor_heap[h].refCount <= 0)
    {
        tensor_heap[h].data.clear();
        tensor_heap[h].data.shrink_to_fit();
        tensor_heap[h].shape.clear();
        tensor_heap[h].shape.shrink_to_fit();
        tensor_heap[h].strides.clear();
        tensor_heap[h].strides.shrink_to_fit();
        tensor_heap[h].freed = true;
        // Free its gradient buffer too, Otherwise every intermediate tensor of every 
        // training step keeps its gradient forever 
        if (h < (int)grad_heap.size())
        {
            grad_heap[h].data.clear();
            grad_heap[h].data.shrink_to_fit();
            grad_heap[h].active = false;
        }
    }
}

// Which mem[] object slots (instance fields, method self copies) currently own a reference to the
// tensor they hold. Only used for ASSIGNs with own_kind 1 or 2 (see compiler.h). Starts all false, so
// a field's initial 0 is never mistaken for an owned handle 0.
static std::vector<char> g_memOwns;
static bool slot_owns(int i) { return i >= 0 && i < (int)g_memOwns.size() && g_memOwns[i]; }
static void set_owns(int i, bool v)
{
    if (i < 0) return;
    if (i >= (int)g_memOwns.size()) g_memOwns.resize(i + 1, 0);
    g_memOwns[i] = v ? 1 : 0;
}

static std::mt19937 rng(42);

struct ProgressBar {
    int total;
    int width;
};

static vector<ProgressBar> progress_heap;

static int ten_zeros(const std::vector<int>& shape) { return alloc_tensor_nd(shape); }

static int ten_ones(const std::vector<int>& shape)
{
    int h = alloc_tensor_nd(shape);
    for (auto& v : tensor_heap[h].data) v = 1.0;
    return h;
}

static int ten_randn(const std::vector<int>& shape)
{
    std::normal_distribution<double> dist(0.0, 1.0);
    int h = alloc_tensor_nd(shape);
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
        const string& funcName = kv.first;
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
        // memo cache is int-only: float/double/tensor returns or params would truncate
        if (is_pure)
        {
            VarType rt = functionReturnType.count(funcName) ? functionReturnType[funcName] : TYPE_UNKNOWN;
            if (rt == TYPE_FLOAT || rt == TYPE_DOUBLE || rt == TYPE_TENSOR) is_pure = false;
        }
        if (is_pure && functionParamTypes.count(funcName))
        {
            for (VarType pt : functionParamTypes[funcName])
            {
                if (pt == TYPE_FLOAT || pt == TYPE_DOUBLE || pt == TYPE_TENSOR) { is_pure = false; break; }
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

        switch (pc->type)  // main instruction dispatch switch
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

                //mem[pc->assign_inst.left_hand_side_index] = result;
                
                {
                    int lhsIdx = pc->assign_inst.left_hand_side_index;
                    int srcIdx = pc->assign_inst.operand1_index;
                    int oldVal = mem[lhsIdx];
                    mem[lhsIdx] = result;
                    if (pc->assign_inst.op == OPERATOR_NONE && pc->assign_inst.own_kind == 1)
                    {
                        // Field store: the field gives up its old tensor and takes its own reference
                        // to the new one, if the value is a tensor. The source slot keeps its reference
                        // (and is released by liveness), so the field must retain. Retain before
                        // releasing so storing the same handle back into the field cannot free it.
                        bool isTensor = g_tensorSlots.count(srcIdx) > 0;
                        if (isTensor) tensor_retain(result);
                        if (slot_owns(lhsIdx)) tensor_release(oldVal);
                        set_owns(lhsIdx, isTensor);
                        pc = pc->next;
                        break;
                    }
                    if (pc->assign_inst.op == OPERATOR_NONE && pc->assign_inst.own_kind == 2)
                    {
                        // Field move (method copy-in / copy-back): the reference
                        // moves with the value; the refcount does not change
                        if (slot_owns(lhsIdx) && oldVal != result) tensor_release(oldVal);
                        set_owns(lhsIdx, slot_owns(srcIdx));
                        set_owns(srcIdx, false);
                        pc = pc->next;
                        break;
                    }
                }

                // A plain copy ("op == OPERATOR_NONE") of a tensor-tainted slot creates a second,
                // simultaneously-live alias of the same tensor_heap entry -- the source slot keeps
                // its value too, it isn't consumed. Without a retain here, the two aliases would share
                // one refcount, and the first of them to go dead (per the compiler's liveness-based TENSOR_RELEASE)
                // insertion) would free the handle out from under the other. An arithmetic op (+, -, *, /) never
                // applies to a tensor handle, so only OPERATOR_NONE needs this.
                if (pc->assign_inst.op == OPERATOR_NONE && g_tensorSlots.count(pc->assign_inst.left_hand_side_index))
                {
                    tensor_retain(mem[pc->assign_inst.left_hand_side_index]);
                }
                pc = pc->next;
                break;

            case CJMP:
                if (pc->cjmp_inst.target == NULL)
                {
                    debug("Error: pc->cjmp_inst->target is null.\n");
                    exit(1);
                }
                // float/double operands live in fmem[]/dmem[], not mem[]. This used to always read mem[]
                // regardless of type, so comparing a flaot/double variable in a condition silently comparing
                // the wrong memory (and for doubles, whose slot indices run past mem[]'s size, could read out of
                // bounds). operand_type is set by the parser per comparison.
                {
                    double d1, d2;
                    switch (pc->cjmp_inst.operand_type)
                    {
                        case TYPE_DOUBLE:
                        {
                            d1 = dmem[pc->cjmp_inst.operand1_index];
                            d2 = dmem[pc->cjmp_inst.operand2_index];
                            break;
                        }
                        case TYPE_FLOAT:
                        {
                            d1 = (double)fmem[pc->cjmp_inst.operand1_index];
                            d2 = (double)fmem[pc->cjmp_inst.operand2_index];
                            break;
                        }
                        default:
                        {
                            d1 = (double)mem[pc->cjmp_inst.operand1_index];
                            d2 = (double)mem[pc->cjmp_inst.operand2_index];
                            break;
                        }
                    }
                    switch (pc->cjmp_inst.condition_op)
                    {
                        case CONDITION_GREATER:
                        {
                            pc = (d1 > d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                        case CONDITION_LESS:
                        {
                            pc = (d1 < d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                        case CONDITION_NOTEQUAL:
                        {
                            pc = (d1 != d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                        case CONDITION_EQUAL:
                        {
                            pc = (d1 == d2) ? pc->next : pc->cjmp_inst.target;
                            break;
                        }
                    }
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
#if defined(_WIN32) && !defined(NOVA_NO_JIT)  // -DNOVA_NO_JIT: always interpret
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
                    case CONDITION_EQUAL: pass = (left == right); break;
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
                        res = ten_zeros({mem[a[0]], mem[a[1]]});
                        break;
                    }
                    case TEN_ONES:
                    {
                        res = ten_ones({mem[a[0]], mem[a[1]]});
                        break;
                    }
                    case TEN_RANDN:
                    {
                        res = ten_randn({mem[a[0]], mem[a[1]]});
                        break;
                    }
                    case TEN_XAVIER:
                    {
                        res = ten_xavier(mem[a[0]], mem[a[1]]);
                        break;
                    }
                    case TEN_GET:
                    {
                        // Returns a double: push to dmem and return the dslot index, same convention
                        // as TEN_SUM/TEN_MEAN/TEN_MAX below. Previously this truncated the value to int,
                        // silently dropping the fraction.
                        if (!tensor_is_live(mem[a[0]]))
                        {
                            fprintf(stderr, "Runtime error at line%d: tensor_get on a freed tensor handle (%d) - "
                                            "this means the compiler's tensor-free pass release it too early; "
                                            "pleae report this as bug.\n", pc->line_no, mem[a[0]]);
                            exit(1);
                        }
                        double v = tensor_heap[mem[a[0]]].at(mem[a[1]], mem[a[2]]);
                        int dslot = (int)dmem.size();
                        dmem.push_back(v);
                        next_double_available++;
                        res = dslot;
                        break;
                    }
                    case TEN_SET:
                    {
                        if (!tensor_is_live(mem[a[0]]))
                        {
                            fprintf(stderr, "Runtime error at line %d: tensor_set on a freed handle (%d) - "
                                            "this means the compiler's tensor-free pass released it too early; "
                                            "please report this as a bug.\n", pc->line_no, mem[a[0]]);
                            exit(1);
                        }
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
                            // Always return a 1x1 tensor handle, matching ag_mse_loss, 
                            // so mse_loss's result type doesn't depend on grad_mode.
                            // (Previously this branch returned a dmemdslot instead, which the parser
                            // typed identically to the tensor-handle brnahc - a real type mismatch 
                            // depending on runtime state.)
                            double loss = ten_mse_loss(mem[a[0]], mem[a[1]]);
                            int h= alloc_tensor(1, 1);
                            tensor_heap[h].at(0, 0) = loss;
                            res = h;
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
                            // same fix as ten_mse_loss, always a 1x1 tensor handle
                            double loss = ten_ce_loss(mem[a[0]], mem[a[1]]);
                            int h = alloc_tensor(1, 1);
                            tensor_heap[h].at(0, 0) = loss;
                            res = h;
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
                    case TEN_RANK:
                    case TEN_DIM:
                    {
                        int th = mem[a[0]];
                        if (!tensor_is_live(th))
                        {
                            fprintf(stderr, "Runtime error at line %d: %s on a freed or invalid tendor handle (%d)\n",
                                    pc->line_no, tc.op == TEN_RANK ? "tensor_rank" : "tensor_dim", th);
                            exit(1);
                        }
                        Tensor& T = tensor_heap[th];
                        if (tc.op == TEN_RANK) { res = T.ndim(); break; }
                        int d = mem[a[1]];
                        if (d < 0 || d >= T.ndim())
                        {
                            fprintf(stderr, "Runtime error at line %d: tensor_dim: dimension %d out of range for a rank-%d tensor\n",
                                    pc->line_no, d, T.ndim());
                            exit(1);
                        }
                        res = T.shape[d];
                        break;
                    }
                    case TEN_LIVE_COUNT:
                    {
                        // number of tensors not yet freed; lets a test assert "no leak"
                        int live = 0;
                        for (auto& t : tensor_heap) if (!t.freed) live++;
                        res = live;
                        break;
                    }
                }
                if (tc.result_slot >= 0)
                {
                    // TEN_SUM/TEN_MEAN/TEN_MAX/TEN_GET push their value to a freshdmem slot and leave 
                    // its index in `res`; the parser allocated result_slot as a double slot 
                    // (alloc_double_slot) for these ops, so copy the value across. Everything else stores
                    // a tensor handle, int result directly in mem[] as before.
                    switch (tc.op)
                    {
                        case TEN_SUM: case TEN_MEAN: case TEN_MAX: case TEN_GET:
                        {
                            dmem[tc.result_slot] = dmem[res];
                            break;
                        }
                        default:
                        {
                            mem[tc.result_slot] = res;
                            break;
                        }
                    }
                }
                pc = pc->next;
                break;
            }

            case TENSOR_RELEASE:
            {
                // Compiler-inserted (see insert_tensor_releases()): this slot's tensor handle is dead 
                // from here on (per liveness) - release this alias. tensor_release() silently no-ops if mem[slot]
                // isn't a live handle; that should never happen given how insertion points are chosen 
                // (see the big comment above insert_tensor_releases()), but the no-op is kept as a defensive fallback rather
                // than a hard error, since a false positive here would halt an otherwise-correct program.
                tensor_release(mem[pc->tensor_release_inst.slot]);
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
        int simplified = algebraic_simplify(program);
        removed += remove_self_copies(program);
        int propogated = copy_propagate(program);
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

    // Compiler-inserted tensor frees (liveness-based, refcounted).
    // Runs after detect_memoizable_functions() so it can strip any tensor-tainted-parameter
    // function back out of pure_functions, and after the --optimize block so it sees the IR that will
    // actually run.
    insert_tensor_releases(program);

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
    {
        dump_ir(program);
        for (auto& kv : functionTable)
        {
            printf("\n-- function IR: %s --\n", kv.first.c_str());
            dump_ir(kv.second);
        }
    }

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

#if defined(_WIN32) && !defined(NOVA_NO_JIT)
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

    execute_program(program);
    fflush(stdout);

    if (getenv("NOVA_TENSOR_STATS"))
    {
        int live = 0, freed = 0;
        for (auto& t : tensor_heap) (t.freed ? freed : live)++;
        fprintf(stderr, "[tensor-stats] heap_size=%zu live=%d freed=%d\n", 
                tensor_heap.size(), live, freed);
    }

    return 0;
}
#endif  // NOVA_NOVM_BUILD
