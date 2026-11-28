// Global interpreter/compiler state shared across modules (formerly the top of compiler.cc).
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
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"

using namespace std;


bool suppress_output = false;

bool debug_mode = false;  // nova --debug

// The ignite block's variables by name (lowering fills them)
map<string, int> symbolTable;
map<string, int> floatSymbolTable;
map<string, int> doubleSymbolTable;

// What lowering writes for the passes after it: each function's body, its parameter and return types and its
// return slots, and the slots that hold tensor handles
map<string, InstructionNode*> functionTable;
map<string, VarType> functionReturnType;
map<string, vector<VarType>> functionParamTypes;
map<string, vector<int>> functionReturnSlots;
set<int> g_tensorSlots;

// Memoization for pure recursive functions
unordered_set<InstructionNode*> pure_functions;
unordered_set<InstructionNode*> recursive_functions;
map<InstructionNode*, map<vector<int>, vector<int>>> memo_cache;

stack<CallFrame> callStack;

// Classes: each class's methods by id, as virtual calls find them, and by name
map<int, map<int, MethodTarget>> virtualTable;
map<string, map<string, InstructionNode*>> classMethodTable;

// Global memory vectors

vector<int> mem;
int next_available = 0;

vector<std::string> strMem;
int next_str_available = 0;

string hf_api_token;


vector<int> freeList;

vector<float> fmem;
int next_float_available = 0;

vector<double> dmem;
int next_double_available = 0;
