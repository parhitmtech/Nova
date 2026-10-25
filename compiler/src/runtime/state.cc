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

// Memoization for pure recursive functions
unordered_set<InstructionNode*> pure_functions;
unordered_set<InstructionNode*> recursive_functions;
map<InstructionNode*, map<vector<int>, vector<int>>> memo_cache;

stack<CallFrame> callStack;

// Add near debug globals
map<string, pair<int, VarType>> debug_symbol_snapshot;  // name -> {slot, type}
int import_line_offset = 0; // lines added by imports - used to skip imported code

// Map Definitions for Class 
map<string, ClassDef> classTable;
map<int, map<int, MethodTarget>> virtualTable;
map<string, string> varClassType;
map<string, map<string, InstructionNode*>> classMethodTable;
map<string, map<string, vector<string>>> classMethodParams;
map<string, map<string, vector<VarType>>> classMethodParamTypes;
map<string, map<string, VarType>> classMethodReturnType;
int currentSelfBase = -1;
string currentSelfName = "";

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
