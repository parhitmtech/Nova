#ifndef NOVA_RUNTIME_H
#define NOVA_RUNTIME_H
// Interpreter, debugger, REPL and import state shared across src/runtime and src/driver.

#include <map>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "lexer.h"
#include "compiler.h"

struct CallFrame {
    struct InstructionNode* returnAddress;
    struct InstructionNode* callSite = nullptr;  // the CALL instruction, for a runtime error's call stack
    int dest_index;
    VarType dest_type = TYPE_UNKNOWN;
    int slot_base;
    std::vector<int> savedSlots;
    std::vector<int> allRetSlots;
    std::vector<VarType> allRetTypes;
    bool is_memoizable = false;
    InstructionNode* memo_func = nullptr;
    std::vector<int> memo_key;
};

// src/runtime/state.cc
extern std::stack<CallFrame> callStack;
extern const struct InstructionNode* runtime_pc;  // the instruction running, for runtime errors (errors.cc)
extern std::string program_file;                    // the file being run, as given (main.cc)
extern bool suppress_output;
extern bool debug_mode;                 // nova --debug: the step debugger (debugger.h) runs the program
extern std::unordered_set<InstructionNode*> pure_functions;
extern std::unordered_set<InstructionNode*> recursive_functions;
extern std::map<InstructionNode*, std::map<std::vector<int>, std::vector<int>>> memo_cache;
extern std::string hf_api_token;

// src/driver/debugger.cc — benchmark mode replays the first run's stdin.
extern std::vector<int> input_replay_buffer;
extern int input_replay_index;          // -1 = live stdin mode

// src/frontend/imports.cc
std::string read_file(const std::string& path);
std::string get_stdlib_path();
// Inlines the modules src brings in with 'orbit'; file is src's name, for diagnostics and "#@line" directives
std::string preprocess_import(const std::string& src, const std::string& base_dir,
                              std::set<std::string>& already_imported, const std::string& file);

// src/runtime/memoize.cc
void detect_memoizable_functions();

// src/driver/repl.cc
void run_repl();

// The ignite block's variables by name, which the debugger shows (src/runtime/state.cc; lowering fills them)
extern std::map<std::string, int> symbolTable;
extern std::map<std::string, int> floatSymbolTable;
extern std::map<std::string, int> doubleSymbolTable;

#endif
