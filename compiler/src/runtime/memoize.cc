// Finds pure recursive functions whose calls can be memoized. Split out of compiler.cc.
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

using namespace std;

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
