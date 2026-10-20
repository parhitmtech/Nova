#ifndef NOVA_PARSER_INTERNAL_H
#define NOVA_PARSER_INTERNAL_H
// Private to src/frontend/parser*.cc: parser state, symbol tables and every parse_* function, so the
// recursive-descent parser can live in several files. Not part of the compiler's public interface.

#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <stack>
#include <set>
#include <fstream>
#include <sstream>
#include "compiler.h"
#include "lexer.h"

using namespace std;

// struct support
struct FieldInfo {
    string name;  // dotted for nested: "a.x"
    VarType type;
    string struct_type;  // non-empty if this field itself is a struct
};

struct StructDef {
    string name;  
    vector<FieldInfo> fields;  // flattened - nested structs expanded inline
};

// ── Parser state (defined in parser_state.cc) ────────────────────────────────
extern vector<string> errorList;
extern bool insideFunction;
extern int currentFuncRetIdx;
extern map<string, int> symbolTable;
extern map<string, int> floatSymbolTable;
extern map<string, int> doubleSymbolTable;
extern std::set<int> g_tensorSlots;
extern std::set<int> g_fieldSlots;
extern std::set<int> g_selfSlots;
extern LexicalAnalyzer lexer;
extern Token token;
extern map<string, InstructionNode*> functionTable;
extern map<string, vector<string>> functionParams;
extern map<string, int> functionReturnIndex;
extern map<string, int> arrayTable;
extern map<string, int> arraySizeTable;
extern map<string, bool> arrayDynamic;
extern map<string, pair<int,int>> functionSlotRange;
extern map<string, VarType> typeTable;
extern map<string, VarType> functionReturnType;
extern map<string, vector<VarType>> functionParamTypes;
extern map<string, vector<int>> functionParamSlots;
extern VarType lastExprType;
extern VarType currentFuncRetType;
extern const map<string, TensorOp> TENSOR_OP_MAP;
extern const set<TensorOp> TENSOR_VOID_OPS;
extern const set<TensorOp> TENSOR_INT_OPS;
extern const set<TensorOp> TENSOR_DOUBLE_OPS;
extern map<string, StructDef> structTable;
extern map<string, string> varStructType;
extern map<string, map<string, int>> structFieldSlots;
extern map<string, map<string, VarType>> structFieldTypes;
extern map<string, int> currentSelfFieldSlots;
extern map<string, VarType> currentSelfFieldTypes;
extern map<string, int> currentSelfFieldArraySizes;
extern map<string, map<string, map<string, int>>> classMethodArraySizes;
extern map<string, map<string, vector<int>>> classMethodParamSlots;
extern string currentSelfClassName;
extern map<string, vector<int>> functionReturnSlots;
extern map<string, vector<VarType>> functionReturnTypesList;
extern vector<int> currentFuncRetSlots;
extern vector<VarType> currentFuncRetTypes;
extern string currentFunctionLabel;
bool types_compatible(VarType value, VarType declared);  // may a value of this type go where that type is declared?
void check_return_type(VarType value, VarType declared, int line);  // error naming currentFunctionLabel

// ── Functions (parser_util.cc, parser_classes.cc, parser_builtins.cc, parser_statements.cc,
//    parser_expressions.cc, parser.cc) and imports.cc ───────────────────────────
string read_file(const string& path);
string get_stdlib_path();
string preprocess_import(const string& src, const string& base_dir, set<string>& already_imported);
string typeToString(VarType t);
VarType type_of_keyword(TokenType t);  // int/bool/string/float/double/Tensor -> its type, else TYPE_UNKNOWN
void report_error(int line, string message);
bool check_declared(string name, int line);
bool name_is_declared(const string& name);  // a variable, array, struct or object with this name is visible
void enter_block();  // block scoping: names declared after this call ...
void exit_block();  // ... are removed by the matching exit_block()
bool check_bounds(string arrName, int index, int line);
bool check_balanced_parens();
void append(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* node);
int alloc_temp();
void expect_condition_lparen(const string& keyword);
void expect_condition_rparen(const string& keyword);
void parse_struct_definition();
void parse_struct_instantiation(const string& structTypeName, struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_class_definition();
void parse_class_instantiation(const string& className, struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_method_call(const string& varName, const string& methodName, struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_tensor_call(const string& name, struct InstructionNode*& head, struct InstructionNode*& tracker);
int emit_tensor_op(TensorOp, const string& name, const vector<int>& args, const vector<VarType>& type,
                   const vector<string>& argNames, int line, struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_hf_call(const string& method, struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_sol_call(const string& method, struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_typed_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_multi_return_assignment(struct InstructionNode*& head, struct InstructionNode*&tracker);
void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_factor(struct InstructionNode*& head, struct InstructionNode*& tracker);
int coerce_float_double(int slot, VarType fromType, VarType toType, struct InstructionNode*& head, struct InstructionNode*& tracker, int lineNo);
int parse_term(struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_expression(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_assignment_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_assignment_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* failTarget);
void emit_truth_test(int v, VarType t, bool wantTrue, struct InstructionNode* failTarget, int line,
                     struct InstructionNode*& head, struct InstructionNode*& tracker);
bool branch_on_comparison(int v, struct InstructionNode* failTarget, struct InstructionNode* tracker);
void parse_if_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_do_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_function_definition();
struct InstructionNode* parse_repl_input(const std::string& s);
struct InstructionNode* parse_generate_intermediate_representation();

#endif
