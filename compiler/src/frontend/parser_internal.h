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
// A vector<T> or map<K, V> variable's types (parser_containers.cc)
struct ContainerInfo
{
    bool isMap = false;
    VarType key = TYPE_UNKNOWN;  // map: TYPE_INT or TYPE_STRING
    VarType elem = TYPE_UNKNOWN;
};
extern map<string, ContainerInfo> containerTable;
// The container type of the last expression parsed, valid when lastExprType is TYPE_VECTOR or TYPE_MAP
// (every expression that gives a container sets it, as every expression sets lastExprType).
extern ContainerInfo lastExprContainer;
extern map<string, vector<ContainerInfo>> functionParamContainers;  // per parameter (used for container ones)
extern map<string, ContainerInfo> functionReturnContainer;  // when the function returns a container
extern ContainerInfo currentFuncRetContainer;  // the function being compiled
extern map<string, map<string, ContainerInfo>> classFieldContainers;  // class -> container field -> type
extern map<string, map<string, vector<ContainerInfo>>> classMethodParamContainers;  // class -> method -> per param
extern map<string, map<string, ContainerInfo>> classMethodReturnContainer;   // class -> method returning a container
int emit_container_new(const ContainerInfo& info, int line, struct InstructionNode*& head, struct InstructionNode *& tracker);
// v[i] = x, v.push(x), v = w, ... on a container (a variable or a field), with token on the '[', '.' or '='
// after its name. Token ends on ';'.
void parse_container_statement_at(int handle, const ContainerInfo& info, const string& name, int line,
                                  struct InstructionNode*& head, struct InstructionNode*& tracker);
bool at_container_type();  // token is 'vector' / 'map' followed by '<'
bool parse_container_type(ContainerInfo& info);  // reads vector<T> / map<K, V>; token ends on '>'
string container_type_name(const ContainerInfo& info);  // "vector<int>"
bool same_container(VarType t, const ContainerInfo& c, const ContainerInfo& want);  // t / c is a `want`
int parse_container_constructor(struct InstructionNode*& head, struct InstructionNode*& tracker);  // vector<int>()
int parse_container_postfix(int handle, const ContainerInfo& info, const string& name, int line,
                            struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_container_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_container_expression(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_container_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
extern map<string, string> varStructType;
extern map<string, map<string, int>> structFieldSlots;
extern map<string, map<string, VarType>> structFieldTypes;
// t[i, j] = value with token on '[': stores one element of the Tensor in slot t (parser_statements.cc)
void parse_tensor_element_store(int t, const string& name, int line,
                                struct InstructionNode*& head, struct InstructionNode*& tracker);
// Class instances as references (parser_objects.cc)
extern int currentSelfHandle;                                    // the slot holding self's handle in a method, else -1
extern map<string, map<string, int>> classMethodSelfHandle;      // class -> method -> its hidden self parameter slot
const ClassFieldInfo* class_field(const string& cls, const string& field, int* index);  // nullptr if none
int emit_object_new(const string& cls, int line, struct InstructionNode*& head, struct InstructionNode*& tracker);
extern string lastExprClass;  // the class of the last expression, when it is an object
extern bool lastExprTemp;     // ... and whether it is a temporary (a new object, a call's result)
extern string currentFuncRetClass;    // the class the function being compiled returns, when it returns an object
extern map<string, vector<string>> functionParamClasses;                  // function -> each parameter's class
extern map<string, string> functionReturnClass;
extern map<string, map<string, vector<string>>> classMethodParamClasses;  // class -> method -> each parameter's class
extern map<string, map<string, string>> classMethodReturnClass;
bool parse_class_type_name(string& cls);  // Point / Box<int> as a type; token ends on its last token
string value_type_name(VarType t, const ContainerInfo& container, const string& cls);
void emit_object_store(int slot, int value, bool temp, int line, struct InstructionNode*& head,
                       struct InstructionNode*& tracker);
void release_object_temps(const vector<int>& temps, int line, struct InstructionNode*& head,
                          struct InstructionNode*& tracker);
// A call's arguments; temps are the ones that are temporary objects, released after the call.
struct CallArgs
{
    vector<int> slots;
    vector<VarType> types;
    vector<ContainerInfo> containers;  // an argument's container type, when it is one
    vector<string> classes;  // an argument's class, when it is an object
    vector<int> temps;
};
CallArgs parse_call_arguments(struct InstructionNode*& head, struct InstructionNode*& tracker);  // token ends past ')'
void check_object_argument(const CallArgs& a, int i, VarType want, const string& wantClass, const string& callee, int line);
int parse_constructor_expression(const string& cls, int line, struct InstructionNode*& head,
                                 struct InstructionNode*& tracker);  // token on '('
void parse_object_value_into(int slot, const string& cls, const string& shown, int line,
                             struct InstructionNode*& head, struct InstructionNode*& tracker);  // token on '='
int object_field_read(int handle, const string& cls, const string& field, int line,
                      struct InstructionNode*& head, struct InstructionNode*& tracker);
void object_field_write(int handle, const string& cls, const string& field, int value, VarType valueType,
                        const ContainerInfo& valueContainer, const string& shown, int line,
                        struct InstructionNode*& head, struct InstructionNode*& tracker);
// obj.f / self.f in an expression, with token on the token after the field name (an array field's [i] included)
int parse_field_expression(int handle, const string& cls, const string& field, int line,
                           struct InstructionNode*& head, struct InstructionNode*& tracker);
// obj.f = x, self.f[i] = x, self.ids.push(x), ... with token on the token after the field name; ends on ';'
void parse_field_statement(int handle, const string& cls, const string& field, const string& shown, int line,
                           struct InstructionNode*& head, struct InstructionNode*& tracker);
int emit_method_call(const string& cls, const string& method, int objectSlot, const vector<int>& args,
                     const vector<VarType>& argTypes, int line, struct InstructionNode*& head, struct InstructionNode*& tracker);

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
void set_variables_aside();  // empty every variable table until restore_variables(): a generic function is
void restore_variables();  // compiled in the middle of its caller and must not see the caller's names
void set_loops_aside();  // likewise for the loops 'break' and 'continue' jump out of
void restore_loops();
bool is_generic_function(const string& name);
int generic_parameter_count(const string& name);
string instantiate_generic_function(const string& name, const vector<VarType>& argTypes, const vector<ContainerInfo>& argContainers,
                                    const vector<string>& argClasses);
void compile_with_caller_aside(int position, void (*compile)());  // compile a definition from the token at position
bool is_generic_class(const string& name);
string parse_generic_class_type();  // Box<int> with token on Box: compiles the copy, returns "Box<int>"; token ends on '>'
void infer_return_type(const vector<VarType>& types, const ContainerInfo& container, const string& cls, int line);
extern bool currentFuncReturnPending;  // compiling a function without '-> type' that has not returned a value yet
extern bool currentFuncSelfCalled;  // ... and it has called itself meanwhile
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
