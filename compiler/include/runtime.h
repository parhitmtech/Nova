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
extern bool suppress_output;
extern bool debug_mode;
extern bool debug_step;                 // pause on every instruction (vs. only at breakpoints)
extern std::set<int> debug_breakpoints;
extern std::unordered_set<InstructionNode*> pure_functions;
extern std::unordered_set<InstructionNode*> recursive_functions;
extern std::map<InstructionNode*, std::map<std::vector<int>, std::vector<int>>> memo_cache;
extern std::map<std::string, std::pair<int, VarType>> debug_symbol_snapshot;  // name -> {slot, type}
extern int import_line_offset;          // lines added by imports (skipped by the debugger)
extern std::string hf_api_token;

// src/driver/debugger.cc — benchmark mode replays the first run's stdin.
extern std::vector<int> input_replay_buffer;
extern int input_replay_index;          // -1 = live stdin mode
void debug_print_state(int line_no);
std::string debug_wait_command();

// src/frontend/imports.cc
std::string read_file(const std::string& path);
std::string get_stdlib_path();
std::string preprocess_import(const std::string& src, const std::string& base_dir,
                              std::set<std::string>& already_imported);

// src/runtime/memoize.cc
void detect_memoizable_functions();

// src/driver/repl.cc
void run_repl();

// Parser state (src/frontend/parser.cc)
extern LexicalAnalyzer lexer;
extern std::vector<std::string> errorList;
extern std::map<std::string, int> symbolTable;
extern std::map<std::string, int> floatSymbolTable;
extern std::map<std::string, int> doubleSymbolTable;
struct InstructionNode* parse_repl_input(const std::string& input);

#endif
