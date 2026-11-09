#ifndef NOVA_REGALLOC_H
#define NOVA_REGALLOC_H
// Liveness analysis, and the liveness-driven tensor release pass that uses it.

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


#endif
