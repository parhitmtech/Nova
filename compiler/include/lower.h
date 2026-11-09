#ifndef NOVA_LOWER_H
#define NOVA_LOWER_H

// Lowering (roadmap phase 2c): the checked tree (check.h) becomes the instructions the interpreter runs (compiler.h),
// and the tables the passes after it read (functionTable and the function tables beside it, classMethodTable,
// virtualTable). Every name and type comes from the check pass's result: lowering keeps no tables by name.
//
// The check pass rejects what lowering does not cover; unlowered() is a last check, so a program the check pass lets
// through by mistake stops with an error instead of being lowered wrongly.

#include <string>
#include <vector>
#include "check.h"
#include "compiler.h"

// A variable a REPL session keeps (repl.cc). Each entry is lowered as a program of its own whose ignite block starts
// with one statement per variable earlier entries made, declaring it again: lowering gives it the slots it has
// instead of lowering that statement, and adds the variables the rest of the block declares to the session.
struct ReplVariable
{
    std::string declaration;  // int x, string s = "", Box<int> b = none, a = array(1)
    int slot = -1;            // as lowering keeps a variable: an array's first slot and size, a struct's fields
    int size = 0;
    bool dynamic = false;
    std::vector<int> fields;
};

uint32_t unlowered(const ParseTree& tree, const CheckResult& checked);  // the first node lowering does not cover
InstructionNode* lower_tree(const ParseTree& tree, const CheckResult& checked,
                            std::vector<ReplVariable>* session = nullptr);

#endif  // NOVA_LOWER_H
