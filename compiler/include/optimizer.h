#ifndef NOVA_OPTIMIZER_H
#define NOVA_OPTIMIZER_H
// IR optimization passes (--optimize) and IR inspection. Each returns how many changes it made.

#include "compiler.h"

// src/opt/constant_fold.cc
int constant_fold(struct InstructionNode* program);
int remove_self_copies(struct InstructionNode* head);
int algebraic_simplify(struct InstructionNode* program);

// src/opt/copy_propagation.cc
int copy_propagate(struct InstructionNode* program);

// src/opt/dead_code.cc
int dead_code_eliminate(struct InstructionNode* program);

// src/opt/licm.cc
int loop_invariant_code_motion(struct InstructionNode* program);

// src/opt/inline.cc  (may replace the program head)
int inline_functions(struct InstructionNode*& program);

// src/ir/dump.cc
int count_ir_nodes(struct InstructionNode* program);
void dump_ir(struct InstructionNode* program);

#endif
