#ifndef NOVA_DEBUGGER_H
#define NOVA_DEBUGGER_H
// The step debugger (nova --debug). The interpreter calls debugger_at before each instruction; the debugger stops at
// a statement when the program is stepping or a breakpoint is there, sends its state (every frame of the call stack,
// with its variables' values) and waits for a command. A front end (the VS Code extension's debug adapter) drives it
// through the program's own stdin and stdout:
//
//   out (events): one line each, "\x1e" + JSON + "\n", among the program's output: the \x1e (ASCII's record
//                 separator) marks where an event starts, even after a print that did not end its line.
//                 {"event":"ready"}  the program is compiled; breakpoints and a first command are expected
//                 {"event":"stopped","reason":"entry|step|breakpoint|pause|exception","text":"...","frames":[...]}
//                 {"event":"input"}  the program called input(): the next line in is its value
//   in (commands): one per line, read as they come, while the program runs too (a thread reads them)
//                 breakpoints <file>\t<line>,<line>...   (replaces that file's, at once; lines are 1-based)
//                 pause                                  (stops the running program at its next instruction)
//                 continue | next | stepIn | stepOut     (at a stop)
//                 stop                                   (ends the program, stopped or running)
//                 The end of input (the front end has gone) ends the program at its next stop or input().
//
// A frame is {"name","file","line","column","variables":[...]}, innermost first; a variable is
// {"name","type","value"} and, for a tensor, a container or an object, "children" (variables too).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "compiler.h"

// What lowering knows that the debugger needs: each function's variables, their slots and where each is in scope
// (from its declaration to the end of its block, as source locations), and each class's fields. Filled only in
// debug mode.
struct DebugVariable
{
    std::string name;
    VarType type = TYPE_UNKNOWN;
    std::string typeName;  // as Nova writes it: int, vector<int>, Box<int>
    int slot = -1;
    uint32_t scopeBegin = 0, scopeEnd = UINT32_MAX;
};

struct DebugFunction
{
    std::string name;  // a method's as Class.method
    int slotBase = -1, slotCount = 0;  // its int slot range, which a call saves (the top level's: -1)
    std::vector<DebugVariable> variables;
};

struct DebugClass
{
    std::string name;
    std::vector<std::string> fields;
    std::vector<VarType> fieldTypes;
};

extern std::vector<DebugFunction> debug_functions;  // [0]: the top level (ignite)
extern std::map<int, DebugClass> debug_classes;      // by the id an object's class has at run time

void debugger_start();                                 // the program is compiled: "ready", then the first command
void debugger_at(InstructionNode* pc);                 // before each instruction
int debugger_read_input();                             // input(), while debugging
void debugger_runtime_error(const std::string& text);  // stops there, so the state can be seen, before it ends

// A container's or an object's contents, for the debugger (their stores are their own files'): a container's
// elements, each with its key as text (a vector's: its index), and their type; an object's class id and fields.
bool containers_describe(int h, bool& isMap, VarType& elem, std::vector<std::pair<std::string, double>>& items);
bool objects_describe(int h, int& cls, std::vector<double>& fields);

#endif
