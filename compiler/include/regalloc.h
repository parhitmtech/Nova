#ifndef NOVA_REGALLOC_H
#define NOVA_REGALLOC_H
// Liveness analysis, interference graph, Chaitin-Briggs graph coloring and spill insertion for the
// x86 backend and the JIT, plus the liveness-driven tensor release pass.

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "compiler.h"

struct LivenessResult {
    std::unordered_map<InstructionNode*, std::unordered_set<int>> live_in;
    std::unordered_map<InstructionNode*, std::unordered_set<int>> live_out;
};

// Control Flow Interference Graph
struct InterferenceGraph {
    std::unordered_map<int, std::unordered_set<int>> adj; // slot -> set of interfering slots

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

struct ColoringResult
{
    std::unordered_map<int, int> color;  // slot - register index (into REGISTERS)
    std::unordered_set<int> spilled;  // slots that couldn't be colored
};

// x86-64 registers available for allocation (rax, rsp, rbp are reserved). Defined in coloring.cc.
extern const std::vector<std::string> REGISTERS;
extern const int K;  // number of available registers

// Per-function colorings, computed once before execution and reused by the JIT.
extern std::unordered_map<InstructionNode*, ColoringResult> g_funcColorings;

// src/regalloc/liveness.cc
bool tensor_call_result_is_mem_slot(TensorOp op);
void get_defs(struct InstructionNode* n, std::unordered_set<int>& defs);
void get_uses(struct InstructionNode* n, std::unordered_set<int>& uses);
void get_successors(struct InstructionNode* n, std::vector<struct InstructionNode*>& succs);
LivenessResult compute_liveness(struct InstructionNode* program);
void dump_liveness(struct InstructionNode*, const LivenessResult&);

// src/opt/tensor_release.cc — insert TENSOR_RELEASE after each tensor slot's last use.
void insert_tensor_releases(struct InstructionNode* program);
// Complete g_tensorSlots (propagate tensor-handle slots through copies and calls). Idempotent;
// insert_tensor_releases() does it too, but optimizer passes need it earlier.
void mark_tensor_slots(struct InstructionNode* program);

// src/regalloc/coloring.cc
InterferenceGraph build_interference_graph(struct InstructionNode* program, const LivenessResult& lr);
void dump_interference_graph(const InterferenceGraph&);
std::unordered_set<int> find_live_across_calls(struct InstructionNode* program, const LivenessResult& lr);
ColoringResult color_graph(InterferenceGraph ig,
                           const std::unordered_set<int>& liveAcrossCallSlots = {},
                           const std::unordered_set<int>& constantSlots = {});
void dump_coloring(const ColoringResult&);
std::unordered_set<int> find_constant_slots(struct InstructionNode* program);
struct InstructionNode* insert_spill_code(struct InstructionNode* program, const ColoringResult& cr);

#endif
