#ifndef NOVA_LOWER_H
#define NOVA_LOWER_H

// Lowering (roadmap phase 2c): the checked tree (check.h) becomes the instructions the interpreter runs (compiler.h).
// It writes the same instructions and slots the old parser writes, and fills the same tables the passes after it
// read (functionTable and the function tables beside it), so the optimizer, the interpreter and the CUDA back end
// do not change. Every name and type comes from the check pass's result: lowering keeps no tables by name.
//
// Lowering covers more of the language at each step of phase 2c. Until it covers all of it, lowers() tells the
// driver whether it covers a program, and the old parser compiles the programs it does not.

#include "check.h"
#include "compiler.h"

bool lowers(const ParseTree& tree, const CheckResult& checked);
InstructionNode* lower_tree(const ParseTree& tree, const CheckResult& checked);

#endif  // NOVA_LOWER_H
