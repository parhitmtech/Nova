#include <cstdlib>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <iostream>
#include <ctype.h>
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

vector<string> errorList;
bool insideFunction = false;
int  currentFuncRetIdx = -1;

map<string, int> symbolTable;
map<string, int> floatSymbolTable;
map<string, int> doubleSymbolTable;
LexicalAnalyzer lexer;
Token token;

map<string, InstructionNode*> functionTable;
map<string, vector<string>> functionParams;
map<string, int> functionReturnIndex;
map<string, int> arrayTable;
map<string, int> arraySizeTable;
map<string, bool> arrayDynamic;  // true = runtime-sized, base is in a mem slot
map<string, pair<int,int>> functionSlotRange;

map<string, VarType> typeTable;
map<string, VarType> functionReturnType;
map<string, vector<VarType>> functionParamTypes;
map<string, vector<int>> functionParamSlots;  // param slot indices, persisted after symbol table restore

VarType lastExprType = TYPE_UNKNOWN;
VarType currentFuncRetType = TYPE_UNKNOWN;

// Tensor built-in dispatch table
static const map<string, TensorOp> TENSOR_OP_MAP = {
    {"tensor", TEN_ALLOC},
    {"tensor_zeros", TEN_ZEROS},
    {"tensor_ones", TEN_ONES},
    {"tensor_randn", TEN_RANDN},
    {"tensor_xavier", TEN_XAVIER},
    {"tensor_get", TEN_GET},
    {"tensor_set", TEN_SET},
    {"tensor_rows", TEN_ROWS},
    {"tensor_cols", TEN_COLS},
    {"tensor_print", TEN_PRINT},
    {"matmul", TEN_MATMUL},
    {"tensor_add", TEN_ADD},
    {"tensor_sub", TEN_SUB},
    {"tensor_mul", TEN_MUL},
    {"tensor_scale", TEN_SCALE},
    {"tensor_T", TEN_T},
    {"tensor_sum", TEN_SUM},
    {"tensor_mean", TEN_MEAN},
    {"tensor_max", TEN_MAX},
    {"tensor_clip", TEN_CLIP},
    {"relu", TEN_RELU},
    {"sigmoid", TEN_SIGMOID},
    {"tanh_t", TEN_TANH},
    {"softmax", TEN_SOFTMAX},
    {"relu_grad", TEN_RELU_GRAD},
    {"sigmoid_grad", TEN_SIGMOID_GRAD},
    {"mse_loss", TEN_MSE_LOSS},
    {"mse_grad", TEN_MSE_GRAD},
    {"bce_grad", TEN_BCE_GRAD},
    {"cross_entropy_loss", TEN_CE_LOSS},
    {"cross_entropy_grad", TEN_CE_GRAD},
    {"tensor_sum_rows",    TEN_SUM_AXIS0},
    {"rand_int", TEN_RAND_INT},
    {"tensor_get_row", TEN_GET_ROW},
    {"tensor_set_row", TEN_SET_ROW},
    {"tensor_slice_rows", TEN_SLICE_ROWS},
    {"tensor_load_csv", TEN_LOAD_CSV},
    {"progress_bar", TEN_PROGRESS_BAR},
    {"progress_update", TEN_PROGRESS_UPDATE},
    {"progress_done", TEN_PROGRESS_DONE},
    {"requires_grad", TEN_REQUIRES_GRAD},
    {"backward", TEN_BACKWARD},
    {"tensor_grad", TEN_TENSOR_GRAD},
    {"zero_grad", TEN_ZERO_GRAD},
    {"grad_step", TEN_GRAD_STEP}
};

// ops that don't return a meaningful value (void)
static const set<TensorOp> TENSOR_VOID_OPS = {
    TEN_SET, TEN_PRINT, TEN_SET_ROW, 
    TEN_PROGRESS_UPDATE, TEN_PROGRESS_DONE,
    TEN_REQUIRES_GRAD, TEN_BACKWARD, TEN_ZERO_GRAD
};

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

map<string, StructDef> structTable;  // struct name -> defnition
map<string, string> varStructType;  // var name -> struct type name
map<string, map<string, int>> structFieldSlots;  // var -> {dotted path -> slot}
map<string, map<string, VarType>> structFieldTypes;  // var -> {dotted path -> type}

// class support - parser local state
map<string, int> currentSelfFieldSlots;
map<string, VarType> currentSelfFieldTypes;
map<string, int> currentSelfFieldArraySizes;
map<string, map<string, map<string, int>>> classMethodArraySizes;
map<string, map<string, vector<int>>> classMethodParamSlots;
string currentSelfClassName = "";

// function multi-return support
map<string, vector<int>>functionReturnSlots;  // multi-return slots per function
map<string, vector<VarType>> functionReturnTypesList;  // multi-return types per function
vector<int> currentFuncRetSlots;  // active function's return slots
vector<VarType> currentFuncRetTypes;  // active function's return types


string typeToString(VarType t)
{
    switch (t) {
        case TYPE_INT: return "int";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_CLASS: return "class";
        default: return "unknown";
    }
}

string read_file(const string& path);
string get_stdlib_path();
string preprocess_import(const string& src, const string& base_dir, set<string>& already_imported);

int parse_expression(struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_term(struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_factor(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_typed_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_assignment_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_assignment_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_if_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_do_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_function_definition();
int parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* noOpNode);
void parse_struct_definition();
void parse_struct_instantiation(const string& structTypeName, struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_class_definition();
void parse_class_instantiation(const string& className, struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_method_call(const string& varName, const string& methodName, struct InstructionNode*& head, struct InstructionNode*& tracker);
int parse_tensor_call(const string& name, struct InstructionNode*& head, struct InstructionNode*& tracker);

void report_error(int line, string message)
{
    errorList.push_back("Error at line " + to_string(line) + ": " + message);
}

bool check_declared(string name, int line)
{
    if (symbolTable.find(name) == symbolTable.end()) {
        report_error(line, "undeclared variable '" + name + "'");
        return false;
    }
    return true;
}

bool check_bounds(string arrName, int index, int line)
{
    if (arraySizeTable.find(arrName) == arraySizeTable.end()) {
        report_error(line, "undeclared array '" + arrName + "'");
        return false;
    }
    int size = arraySizeTable[arrName];
    if (index < 0 || index >= size) {
        report_error(line, "array '" + arrName + "' index " + to_string(index) + " out of bounds (size " + to_string(size) + ")");
        return false;
    }
    return true;
}

bool check_balanced_parens()
{
    struct BracketFrame { char bracket; int line; };
    vector<BracketFrame> stk;
    bool valid = true;
    int i = 1;
    Token t = lexer.peek(i);
    while (t.token_type != END_OF_FILE) {
        if (t.token_type == LPAREN || t.token_type == LBRAC || t.token_type == LBRACE) {
            char c = (t.token_type == LPAREN) ? '(' : (t.token_type == LBRAC) ? '[' : '{';
            stk.push_back({c, t.line_no});
        } else if (t.token_type == RPAREN || t.token_type == RBRAC || t.token_type == RBRACE) {
            char expected_open = (t.token_type == RPAREN) ? '(' : (t.token_type == RBRAC) ? '[' : '{';
            char close_char    = (t.token_type == RPAREN) ? ')' : (t.token_type == RBRAC) ? ']' : '}';
            if (stk.empty()) { report_error(t.line_no, string("unmatched closing '") + close_char + "'"); valid = false; break; }
            BracketFrame top = stk.back(); stk.pop_back();
            if (top.bracket != expected_open) {
                report_error(t.line_no, string("mismatched bracket: opened '") + top.bracket + "' on line " + to_string(top.line) + " but closed with '" + close_char + "'");
                valid = false; break;
            }
        }
        i++; t = lexer.peek(i);
    }
    if (valid && !stk.empty()) {
        for (auto& frame : stk) report_error(frame.line, string("unclosed '") + frame.bracket + "' - missing closing bracket");
        valid = false;
    }
    return valid;
}

static void append(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* node)
{
    if (head == nullptr) head = tracker = node;
    else { tracker->next = node; tracker = node; }
}

static int alloc_temp()
{
    return alloc_slot();
}

// CALL after consuming the keyword (WHILE / IF).
// IF the next token is '(', consume it and return true (so we know to consume ')'after condition).
static bool maybe_consume_lparen()
{
    if (token.token_type == LPAREN)
    {
        token = lexer.GetToken();  // consume '('
        return true;
    }
    return false;
}

void parse_struct_definition()
{
    token = lexer.GetToken();
    string structName = token.lexeme;
    int structLine = token.line_no;

    if (structTable.count(structName))
    {
        report_error(structLine, "duplicate struct definition '" + structName + "'");
    }

    StructDef def;
    def.name = structName;

    token = lexer.GetToken();  // '{'
    token = lexer.GetToken();  // first field or '}'

    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        VarType fieldType = TYPE_UNKNOWN;
        string fieldStructType = "";

        if (token.token_type == INT_TYPE) { fieldType = TYPE_INT; token = lexer.GetToken(); }
        else if (token.token_type == BOOL_TYPE) { fieldType = TYPE_BOOL; token = lexer.GetToken(); }
        else if (token.token_type == STRING_TYPE) { fieldType = TYPE_STRING; token = lexer.GetToken(); }
        else if (token.token_type == FLOAT_TYPE) { fieldType = TYPE_FLOAT; token = lexer.GetToken(); }
        else if (token.token_type == DOUBLE_TYPE) { fieldType = TYPE_DOUBLE; token = lexer.GetToken(); }
        else if (token.token_type == ID && structTable.count(token.lexeme))
        {
            fieldStructType = token.lexeme;
            token = lexer.GetToken();  // field name
        }
        else
        {
            report_error(token.line_no, "unknown field type '" + token.lexeme + "'");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            {
                token = lexer.GetToken();
            }
            token = lexer.GetToken();
            continue;
        }

        string fieldName = token.lexeme;

        if (!fieldStructType.empty())
        {
            // nested struct - flatten with prefix fieldName.subfield
            for (auto& nestedField : structTable[fieldStructType].fields)
            {
                FieldInfo fi;
                fi.name = fieldName + "." + nestedField.name;
                fi.type = nestedField.type;
                fi.struct_type = nestedField.struct_type;
                def.fields.push_back(fi);
            }
        }
        else
        {
            FieldInfo fi;
            fi.name = fieldName;
            fi.type = fieldType;
            fi.struct_type = "";
            def.fields.push_back(fi);
        }

        token = lexer.GetToken();  // ';'
        token = lexer.GetToken();  // next field or '}'
    }
    // token = '}'
    structTable[structName] = def;
}

void parse_struct_instantiation(const string& structTypeName, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // ON ENTRY: token = struct type name (already identified by caller)
    token = lexer.GetToken();  // variable name
    string varName = token.lexeme;
    int varLine = token.line_no;

    if (varStructType.count(varName))
    {
        report_error(varLine, "redeclaration of struct variable '" + varName + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return; 
    }

    StructDef& def = structTable[structTypeName];
    varStructType[varName] = structTypeName;

    // allocate one slot per field in the correct memory array
    map<string, int> fieldSlots;
    map<string, VarType> fieldTypes;

    for (auto& field : def.fields)
    {
        int slot = 0;
        if (field.type == TYPE_FLOAT) slot = alloc_float_slot();
        else if (field.type == TYPE_DOUBLE) slot = alloc_double_slot();
        else slot = alloc_slot();
        fieldSlots[field.name] = slot;
        fieldTypes[field.name] = field.type;
    }

    structFieldSlots[varName] = fieldSlots;
    structFieldTypes[varName] = fieldTypes;

    token = lexer.GetToken();  // '=' or ';'

    if (token.token_type == EQUAL)
    {
        // literal init: Point p = {3.14, 2.71} ;
        token = lexer.GetToken();  // '{'
        token = lexer.GetToken();  // first value

        for (int i = 0;i < (int)def.fields.size();i++)
        {
            auto& field = def.fields[i];
            int resultIdx = parse_expression(head, tracker);
            int slot = fieldSlots[field.name];

            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            if (field.type == TYPE_FLOAT)
            {
                node->type = ASSIGN_F;
                node->assign_f_inst.left_hand_side_index = slot;
                node->assign_f_inst.operand1_index = resultIdx;
                node->assign_f_inst.op = OPERATOR_NONE;
            }
            else if (field.type == TYPE_DOUBLE)
            {
                node->type = ASSIGN_D;
                node->assign_d_inst.left_hand_side_index = slot;
                node->assign_d_inst.operand1_index = resultIdx;
                node->assign_d_inst.op = OPERATOR_NONE;
            }
            else
            {
                node->type = ASSIGN;
                node->assign_inst.left_hand_side_index = slot;
                node->assign_inst.operand1_index = resultIdx;
                node->assign_inst.op = OPERATOR_NONE;
            }
            append(head, tracker, node);

            if (token.token_type == COMMA) token = lexer.GetToken();
        }
        // token = '}'
        token = lexer.GetToken();  // ';'
    }
    // else token = ';'
}

void parse_class_definition()
{
    token = lexer.GetToken();  // class name
    string className = token.lexeme;
    int classLine = token.line_no;

    if (classTable.count(className))
    {
        report_error(classLine, "duplicate class definition '" + className + "'");
    }

    ClassDef def;
    def.name = className;
    def.parent = "";

    token = lexer.GetToken();  // EXTENDS or '{'

    if (token.token_type == EXTENDS)
    {
        token = lexer.GetToken();  // parent name
        string parentName = token.lexeme;
        if (!classTable.count(parentName))
        {
            report_error(token.line_no, "unknown parent class '" + parentName + "'");
        }
        else
        {
            def.parent = parentName;
            for (auto& f : classTable[parentName].fields)
            {
                def.fields.push_back(f);
            }
        }
        token = lexer.GetToken();  // '{'
    }

    token = lexer.GetToken();  // first member of '}'

    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        if (token.token_type == DEF)
        {
            token = lexer.GetToken();  // method name
            string methodName = token.lexeme;
            def.methods.push_back(methodName);

            string fullName = className + "::" + methodName;
            vector<string> params;
            vector<VarType> paramTypes;

            // save symbol tables — restored after body so method-scoped names don't leak
            map<string, int>     savedSymbolTable       = symbolTable;
            map<string, VarType> savedTypeTable         = typeTable;
            map<string, int>     savedFloatSymbolTable  = floatSymbolTable;
            map<string, int>     savedDoubleSymbolTable = doubleSymbolTable;

            token = lexer.GetToken();  // '('
            token = lexer.GetToken();  // first param or ')'
            while (token.token_type != RPAREN)
            {
                VarType pType = TYPE_UNKNOWN;
                if (token.token_type == INT_TYPE) { pType = TYPE_INT; token = lexer.GetToken(); }
                else if (token.token_type == BOOL_TYPE) { pType = TYPE_BOOL; token = lexer.GetToken(); }
                else if (token.token_type == STRING_TYPE) { pType = TYPE_STRING; token = lexer.GetToken(); }
                else if (token.token_type == FLOAT_TYPE) { pType = TYPE_FLOAT; token = lexer.GetToken(); }
                else if (token.token_type == DOUBLE_TYPE) { pType = TYPE_DOUBLE; token = lexer.GetToken(); }
                if (token.token_type == ID)
                {
                    params.push_back(token.lexeme);
                    paramTypes.push_back(pType);
                    int pSlot = (pType == TYPE_FLOAT) ? alloc_float_slot() : 
                                (pType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
                    if (pType == TYPE_FLOAT) floatSymbolTable[token.lexeme] = pSlot;
                    else if (pType == TYPE_DOUBLE) doubleSymbolTable[token.lexeme] = pSlot;
                    else symbolTable[token.lexeme] = pSlot;
                    typeTable[token.lexeme] = pType;
                    classMethodParamSlots[className][methodName].push_back(pSlot);
                }
                token = lexer.GetToken();
                if (token.token_type == COMMA) token = lexer.GetToken();
            }
            classMethodParams[className][methodName] = params;
            classMethodParamTypes[className][methodName] = paramTypes;

            token = lexer.GetToken();  // ARROW or '{'
            VarType retType = TYPE_UNKNOWN;
            if (token.token_type == ARROW)
            {
                token = lexer.GetToken();
                if (token.token_type == INT_TYPE) retType = TYPE_INT;
                else if (token.token_type == BOOL_TYPE) retType = TYPE_BOOL;
                else if (token.token_type == STRING_TYPE) retType = TYPE_STRING;
                else if (token.token_type == FLOAT_TYPE) retType = TYPE_FLOAT;
                else if (token.token_type == DOUBLE_TYPE) retType = TYPE_DOUBLE;
                else if (token.token_type == ID && classTable.count(token.lexeme)) retType = TYPE_CLASS;
                token = lexer.GetToken();  // '{'
            }
            classMethodReturnType[className][methodName] = retType;

            int retIdx = (retType == TYPE_FLOAT) ? alloc_float_slot() : 
                         (retType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            functionReturnIndex[fullName] = retIdx;

            // allocate self field slots for this method
            map<string, int> selfSlots;
            map<string, VarType> selfTypes;
            map<string, int> arraySizes;
            for (auto& f : def.fields)
            {
                int s = 0;
                if (f.array_size > 0)
                {
                    s = alloc_slot();  // ONE pointer slot, not N slots
                    arraySizes[f.name] = f.array_size;
                }
                else if (f.type == TYPE_FLOAT) s = alloc_float_slot();
                else if (f.type == TYPE_DOUBLE) s = alloc_double_slot();
                else s = alloc_slot();
                selfSlots[f.name] = s;
                selfTypes[f.name] = f.type; 
            }
            classMethodSelfSlots[className][methodName] = selfSlots;
            classMethodSelfTypes[className][methodName] = selfTypes;
            classMethodArraySizes[className][methodName] = arraySizes;

            // save + set parser context
            bool savedInsideFunc = insideFunction;
            int savedRetIdx = currentFuncRetIdx;
            VarType savedRetType = currentFuncRetType;
            string savedSelfClass = currentSelfClassName;
            map<string, int> savedSelfSlots = currentSelfFieldSlots;
            map<string, VarType> savedSelfTypes = currentSelfFieldTypes;
            map<string, int> savedSelfArraySizes = currentSelfFieldArraySizes;

            insideFunction = true;
            currentFuncRetIdx = retIdx;
            currentFuncRetType = retType;
            currentSelfClassName = className;
            currentSelfFieldSlots = selfSlots;
            currentSelfFieldTypes = selfTypes;
            currentSelfFieldArraySizes = arraySizes;
            classTable[className] = def;

            token = lexer.GetToken();  // first token in body
            struct InstructionNode* mhead = nullptr;
            struct InstructionNode* mtracker = nullptr;

            while (token.token_type != RBRACE)
            {
                if (token.token_type == RETURN)
                {
                    token = lexer.GetToken();
                    int resultIdx = parse_expression(mhead, mtracker);

                    struct InstructionNode* an = new InstructionNode();
                    an->line_no = token.line_no;
                    if (retType == TYPE_FLOAT)
                    {
                        an->type = ASSIGN_F;
                        an->assign_f_inst.left_hand_side_index = retIdx;
                        an->assign_f_inst.operand1_index = resultIdx;
                        an->assign_f_inst.op = OPERATOR_NONE;
                    }
                    else if (retType == TYPE_DOUBLE)
                    {
                        an->type = ASSIGN_D;
                        an->assign_d_inst.left_hand_side_index = retIdx;
                        an->assign_d_inst.operand1_index = resultIdx;
                        an->assign_d_inst.op = OPERATOR_NONE;
                    }
                    else
                    {
                        an->type = ASSIGN;
                        an->assign_inst.left_hand_side_index = retIdx;
                        an->assign_inst.operand1_index = resultIdx;
                        an->assign_inst.op = OPERATOR_NONE;
                    }
                    append(mhead, mtracker, an);

                    struct InstructionNode* rn = new InstructionNode();
                    rn->line_no = token.line_no;
                    rn->type = RET;
                    rn->ret_inst.ret_val_index = retIdx;
                    append(mhead, mtracker, rn);
                    token = lexer.GetToken();  // advance past ';'
                }
                else
                {
                    parse_statement(mhead, mtracker);
                    token = lexer.GetToken();
                }
            }

            // restore context
            insideFunction = savedInsideFunc;
            currentFuncRetIdx = savedRetIdx;
            currentFuncRetType = savedRetType;
            currentSelfClassName = savedSelfClass;
            currentSelfFieldSlots = savedSelfSlots;
            currentSelfFieldTypes = savedSelfTypes;
            currentSelfFieldArraySizes = savedSelfArraySizes;

            symbolTable       = savedSymbolTable;
            typeTable         = savedTypeTable;
            floatSymbolTable  = savedFloatSymbolTable;
            doubleSymbolTable = savedDoubleSymbolTable;

            struct InstructionNode* fb = new InstructionNode();
            fb->line_no = token.line_no;
            fb->type = RET;
            fb->ret_inst.ret_val_index = retIdx;
            append(mhead, mtracker, fb);

            classMethodTable[className][methodName] = mhead;
            token = lexer.GetToken();  // advance past method body '}'
        }
        else if (token.token_type == INT_TYPE || token.token_type == BOOL_TYPE ||
                 token.token_type == STRING_TYPE || token.token_type == FLOAT_TYPE ||
                 token.token_type == DOUBLE_TYPE)
        {
            VarType fieldType = TYPE_UNKNOWN;
            if      (token.token_type == INT_TYPE)    fieldType = TYPE_INT;
            else if (token.token_type == BOOL_TYPE)   fieldType = TYPE_BOOL;
            else if (token.token_type == STRING_TYPE) fieldType = TYPE_STRING;
            else if (token.token_type == FLOAT_TYPE)  fieldType = TYPE_FLOAT;
            else if (token.token_type == DOUBLE_TYPE) fieldType = TYPE_DOUBLE;

            token = lexer.GetToken();  // field name
            ClassFieldInfo fi;
            fi.name = token.lexeme;
            fi.type = fieldType;
            fi.struct_type = "";
            fi.array_size = 0;

            token = lexer.GetToken();  // ';' or '['
            if (token.token_type == LBRAC)
            {
                token = lexer.GetToken();  // size number
                fi.array_size = stoi(token.lexeme);
                token = lexer.GetToken();  // ']'
                token = lexer.GetToken();  // ';'
            }
            def.fields.push_back(fi);
            token = lexer.GetToken();  // next member or '}'
        }
        else if (token.token_type == ID && classTable.count(token.lexeme))
        {
            // nested class type field: ClassName fieldName ;
            string nestedClass = token.lexeme;
            token = lexer.GetToken();  // field name
            string fieldName = token.lexeme;
            for (auto& nf : classTable[nestedClass].fields)
            {
                ClassFieldInfo fi;
                fi.name = fieldName + "." + nf.name;
                fi.type = nf.type;
                fi.struct_type = nf.struct_type;
                def.fields.push_back(fi);
            }
            token = lexer.GetToken();  // ';'
            token = lexer.GetToken();  // next member or '}'
        }
        else
        {
            report_error(token.line_no, "unexpected token in class body: '" + token.lexeme + "'");
            while (token.token_type != SEMICOLON && token.token_type != RBRACE && token.token_type != END_OF_FILE)
                token = lexer.GetToken();
            if (token.token_type == SEMICOLON) token = lexer.GetToken();
        }
    }
    // token = '}'
    classTable[className] = def;
}

void parse_class_instantiation(const string& className, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // ON ENTRY: token = class type name (already consumed by caller's peek detection)
    token = lexer.GetToken();  // variable name
    string varName = token.lexeme;
    int varLine = token.line_no;

    if (varClassType.count(varName))
    {
        report_error(varLine, "redeclaration of class variable '" + varName + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            token = lexer.GetToken();
        return;
    }

    if (!classTable.count(className))
    {
        report_error(varLine, "unknown class '" + className + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            token = lexer.GetToken();
        return;
    }

    ClassDef& def = classTable[className];
    varClassType[varName] = className;
    typeTable[varName] = TYPE_CLASS;
    symbolTable[varName] = alloc_slot();

    map<string, int> fieldSlots;
    map<string, VarType> fieldTypes;
    for (auto& field : def.fields)
    {
        int slot = 0;
        if (field.array_size > 0)
        {
            slot = next_available;
            for (int i = 0;i < field.array_size; i++) alloc_slot();
        }
        else if (field.type == TYPE_FLOAT) slot = alloc_float_slot();
        else if (field.type == TYPE_DOUBLE) slot = alloc_double_slot();
        else slot = alloc_slot();
        fieldSlots[field.name] = slot;
        fieldTypes[field.name] = field.type;
    }
    classFieldSlots[varName] = fieldSlots;
    classFieldTypes[varName] = fieldTypes;

    token = lexer.GetToken();  // '=' or ';'
    if (token.token_type == LPAREN)
    {
        // constructor call: MyClass obj(arg1, arg2) ; 
        token = lexer.GetToken();  // past '('
        vector<int> argIndices;
        vector<VarType> argTypes;
        while (token.token_type != RPAREN)
        {
            int argIdx = parse_expression(head, tracker);
            argIndices.push_back(argIdx);
            argTypes.push_back(lastExprType);
            if (token.token_type == COMMA)
            {
                token = lexer.GetToken();
            }
        }
        token = lexer.GetToken();  // past ')'

        if (!classMethodTable.count(className) || !classMethodTable[className].count("init"))
        {
            report_error(varLine, "class '" + className + "' has no constructor 'init'");
        }
        else
        {
            string fullName = className + "::init";
            map<string, int>& selfSlots = classMethodSelfSlots[className]["init"];
            map<string, VarType>& selfTypes = classMethodSelfTypes[className]["init"];
            map<string, int>& initArrSizes = classMethodArraySizes[className]["init"];
            vector<string>& params = classMethodParams[className]["init"];

            // copy caller field slots into self slots
            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName)) continue;
                int callerSlot = classFieldSlots[varName][fieldName];
                // Array field: store caller's base index as literal pointer
                if (initArrSizes.count(fieldName))
                {
                    int ptrSlot = alloc_slot();
                    mem[ptrSlot] = callerSlot;
                    struct InstructionNode* copyIn = new InstructionNode();
                    copyIn->line_no = token.line_no;
                    copyIn->type = ASSIGN;
                    copyIn->assign_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_inst.operand1_index       = ptrSlot;
                    copyIn->assign_inst.op                   = OPERATOR_NONE;
                    append(head, tracker, copyIn);
                    continue;
                }
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyIn = new InstructionNode();
                copyIn->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyIn->type = ASSIGN_F;
                    copyIn->assign_f_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_f_inst.operand1_index = callerSlot;
                    copyIn->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyIn->type = ASSIGN_D;
                    copyIn->assign_d_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_d_inst.operand1_index = callerSlot;
                    copyIn->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyIn->type = ASSIGN;
                    copyIn->assign_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_inst.operand1_index = callerSlot;
                    copyIn->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyIn);
            }

            // emit CALL
            struct InstructionNode* callNode = new InstructionNode();
            callNode->line_no = token.line_no;
            callNode->type = CALL;
            callNode->call_inst.function_head = classMethodTable[className]["init"];
            callNode->call_inst.ret_val_index = functionReturnIndex[fullName];
            callNode->call_inst.func_slot_base = 0;
            callNode->call_inst.func_slot_count = 0;
            callNode->call_inst.num_params = (int)params.size();
            callNode->call_inst.param_types = nullptr;
            callNode->call_inst.arg_types = nullptr;
            callNode->call_inst.all_ret_slots = nullptr;
            callNode->call_inst.num_ret_slots = 0;
            if (!params.empty())
            {
                int* pSlots = new int[params.size()];
                int* aSlots = new int[params.size()];
                vector<int>& storedParamSlots = classMethodParamSlots[className]["init"];
                for (int i = 0;i < (int)params.size();i++)
                {
                    pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
                    aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
                }
                callNode->call_inst.param_slots = pSlots;
                callNode->call_inst.arg_val_slots = aSlots;
            }
            else
            {
                callNode->call_inst.param_slots = nullptr;
                callNode->call_inst.arg_val_slots = nullptr;
            }
            append(head, tracker, callNode);

            // copy self slots back to caller field slots
            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName))
                {
                    continue;
                }
                // Array fields write directly to caller via dynamic_base — no copy-back needed
                if (initArrSizes.count(fieldName)) continue;
                int callerSlot = classFieldSlots[varName][fieldName];
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyBack = new InstructionNode();
                copyBack->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyBack->type = ASSIGN_F;
                    copyBack->assign_f_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_f_inst.operand1_index = selfSlot;
                    copyBack->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyBack->type = ASSIGN_D;
                    copyBack->assign_d_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_d_inst.operand1_index = selfSlot;
                    copyBack->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyBack->type = ASSIGN;
                    copyBack->assign_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_inst.operand1_index = selfSlot;
                    copyBack->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyBack);
            }
        }
        // token = ';'
    }
    else if (token.token_type == EQUAL)
    {
        token = lexer.GetToken();  // '{'
        token = lexer.GetToken();  // first value

        for (int i = 0; i < (int)def.fields.size(); i++)
        {
            auto& field = def.fields[i];
            int resultIdx = parse_expression(head, tracker);
            int slot = fieldSlots[field.name];

            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            if (field.type == TYPE_FLOAT)
            {
                node->type = ASSIGN_F;
                node->assign_f_inst.left_hand_side_index = slot;
                node->assign_f_inst.operand1_index = resultIdx;
                node->assign_f_inst.op = OPERATOR_NONE;
            }
            else if (field.type == TYPE_DOUBLE)
            {
                node->type = ASSIGN_D;
                node->assign_d_inst.left_hand_side_index = slot;
                node->assign_d_inst.operand1_index = resultIdx;
                node->assign_d_inst.op = OPERATOR_NONE;
            }
            else
            {
                node->type = ASSIGN;
                node->assign_inst.left_hand_side_index = slot;
                node->assign_inst.operand1_index = resultIdx;
                node->assign_inst.op = OPERATOR_NONE;
            }
            append(head, tracker, node);

            if (token.token_type == COMMA) token = lexer.GetToken();
        }
        // token = '}'
        token = lexer.GetToken();  // ';'
    }
    else if (token.token_type == SEMICOLON)
    {
        // auto-call no-arg init() if it exists
        if (classMethodTable.count(className) && classMethodTable[className].count("init") && classMethodParams[className]["init"].empty())
        {
            string fullName = className + "::init";
            map<string, int>& selfSlots = classMethodSelfSlots[className]["init"];
            map<string, VarType>& selfTypes = classMethodSelfTypes[className]["init"];

            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName))
                {
                    continue;
                }
                int callerSlot = classFieldSlots[varName][fieldName];
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyIn = new InstructionNode();
                copyIn->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyIn->type = ASSIGN_F;
                    copyIn->assign_f_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_f_inst.operand1_index = callerSlot;
                    copyIn->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyIn->type = ASSIGN_D;
                    copyIn->assign_d_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_d_inst.operand1_index = callerSlot;
                    copyIn->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyIn->type = ASSIGN;
                    copyIn->assign_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_inst.operand1_index = callerSlot;
                    copyIn->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyIn);
            }

            struct InstructionNode* callNode = new InstructionNode();
            callNode->line_no = token.line_no;
            callNode->type = CALL;
            callNode->call_inst.function_head = classMethodTable[className]["init"];
            callNode->call_inst.ret_val_index = functionReturnIndex[fullName];
            callNode->call_inst.func_slot_base = 0;
            callNode->call_inst.func_slot_count = 0;
            callNode->call_inst.num_params = 0;
            callNode->call_inst.param_slots = nullptr;
            callNode->call_inst.arg_val_slots = nullptr;
            callNode->call_inst.param_types = nullptr;
            callNode->call_inst.arg_types = nullptr;
            callNode->call_inst.all_ret_slots = nullptr;
            callNode->call_inst.num_ret_slots = 0;
            append(head, tracker, callNode);

            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName))
                {
                    continue;
                }
                int callerSlot = classFieldSlots[varName][fieldName];
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyBack = new InstructionNode();
                copyBack->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyBack->type = ASSIGN_F;
                    copyBack->assign_f_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_f_inst.operand1_index = selfSlot;
                    copyBack->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyBack->type = ASSIGN_D;
                    copyBack->assign_d_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_d_inst.operand1_index = selfSlot;
                    copyBack->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyBack->type = ASSIGN;
                    copyBack->assign_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_inst.operand1_index = selfSlot;
                    copyBack->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyBack);
            }
        }
    }
    // else token = ';'
}

int parse_method_call(const string& varName, const string& methodName, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // ON ENTRY: token = '('
    string className = varClassType[varName];

    // walk inheritance chain to find the method (supports inherited + overridden methods)
    string lookupClass = className;
    while (true)
    {
        if (classMethodTable.count(lookupClass) && classMethodTable[lookupClass].count(methodName))
        {
            break;
        }
        if (classTable.count(lookupClass) && !classTable[lookupClass].parent.empty())
        {
            lookupClass = classTable[lookupClass].parent;
        }
        else
        {
            report_error(token.line_no, "class '" + className + "' has no method '" + methodName + "'");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            {
                token = lexer.GetToken();
            }
            lastExprType = TYPE_UNKNOWN;
            return 0;
        }
    }

    token = lexer.GetToken();  // past '('
    // parse arguments
    vector<int>     argIndices;
    vector<VarType> argTypes;
    while (token.token_type != RPAREN)
    {
        int argIdx = parse_expression(head, tracker);
        argIndices.push_back(argIdx);
        argTypes.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'

    // Copy caller's field values into the method's self slots (pass self by copy)
    map<string, int>& selfSlots = classMethodSelfSlots[lookupClass][methodName];
    map<string, VarType>& selfTypes = classMethodSelfTypes[lookupClass][methodName];
    map<string, int>& arrSizes = classMethodArraySizes[lookupClass][methodName];
    for (auto& kv : selfSlots)
    {
        const string& fieldName = kv.first;
        int selfSlot = kv.second;
        if (!classFieldSlots.count(varName) || !classFieldSlots[varName].count(fieldName)) continue;
        int callerSlot = classFieldSlots[varName][fieldName];
        // Array field: store the caller's base index as a literal pointer
        if (arrSizes.count(fieldName))
        {
            int ptrSlot = alloc_slot();
            mem[ptrSlot] = callerSlot;
            struct InstructionNode* copyIn = new InstructionNode();
            copyIn->line_no = token.line_no;
            copyIn->type = ASSIGN;
            copyIn->assign_inst.left_hand_side_index = selfSlot;
            copyIn->assign_inst.operand1_index       = ptrSlot;
            copyIn->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, copyIn);
            continue;
        }
        VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
        struct InstructionNode* copyIn = new InstructionNode();
        copyIn->line_no = token.line_no;
        if (fType == TYPE_FLOAT)
        {
            copyIn->type = ASSIGN_F;
            copyIn->assign_f_inst.left_hand_side_index = selfSlot;
            copyIn->assign_f_inst.operand1_index = callerSlot;
            copyIn->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fType == TYPE_DOUBLE)
        {
            copyIn->type = ASSIGN_D;
            copyIn->assign_d_inst.left_hand_side_index = selfSlot;
            copyIn->assign_d_inst.operand1_index = callerSlot;
            copyIn->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            copyIn->type = ASSIGN;
            copyIn->assign_inst.left_hand_side_index = selfSlot;
            copyIn->assign_inst.operand1_index = callerSlot;
            copyIn->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, copyIn);
    }

    // Emit CALL node
    string fullName = lookupClass + "::" + methodName;
    vector<string>& params = classMethodParams[lookupClass][methodName];

    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = token.line_no;
    callNode->type = CALL;
    callNode->call_inst.function_head   = classMethodTable[lookupClass][methodName];
    callNode->call_inst.ret_val_index   = functionReturnIndex[fullName];
    callNode->call_inst.func_slot_base  = 0;
    callNode->call_inst.func_slot_count = 0;
    callNode->call_inst.num_params      = (int)params.size();
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;
    if (!params.empty())
    {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        vector<int>& storedParamSlots = classMethodParamSlots[lookupClass][methodName];
        for (int i = 0; i < (int)params.size(); i++)
        {
            pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        VarType* ptypes = new VarType[params.size()];
        vector<VarType>& storedParamTypes = classMethodParamTypes[lookupClass][methodName];
        for (int i = 0; i < (int)params.size(); i++)
            ptypes[i] = (i < (int)storedParamTypes.size()) ? storedParamTypes[i] : TYPE_UNKNOWN;

        VarType* atypes = new VarType[params.size()];
        for (int i = 0; i < (int)argTypes.size(); i++)
            atypes[i] = (i < (int)argTypes.size()) ? argTypes[i] : TYPE_UNKNOWN;

        callNode->call_inst.param_types   = ptypes;
        callNode->call_inst.arg_types     = atypes;
        callNode->call_inst.param_slots   = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    }
    else
    {
        callNode->call_inst.param_slots   = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
    }
    append(head, tracker, callNode);

    // Copy self slots back to caller's field slots (method may mutate self)
    for (auto& kv : selfSlots)
    {
        const string& fieldName = kv.first;
        int selfSlot = kv.second;
        if (!classFieldSlots.count(varName) || !classFieldSlots[varName].count(fieldName)) continue;
        // Array fields write directly to caller via dynamic_base — no copy-back needed
        if (arrSizes.count(fieldName)) continue;
        int callerSlot = classFieldSlots[varName][fieldName];
        VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
        struct InstructionNode* copyBack = new InstructionNode();
        copyBack->line_no = token.line_no;
        if (fType == TYPE_FLOAT)
        {
            copyBack->type = ASSIGN_F;
            copyBack->assign_f_inst.left_hand_side_index = callerSlot;
            copyBack->assign_f_inst.operand1_index = selfSlot;
            copyBack->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fType == TYPE_DOUBLE)
        {
            copyBack->type = ASSIGN_D;
            copyBack->assign_d_inst.left_hand_side_index = callerSlot;
            copyBack->assign_d_inst.operand1_index = selfSlot;
            copyBack->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            copyBack->type = ASSIGN;
            copyBack->assign_inst.left_hand_side_index = callerSlot;
            copyBack->assign_inst.operand1_index = selfSlot;
            copyBack->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, copyBack);
    }

    // Copy return value to a fresh slot
    VarType mRetType = (classMethodReturnType.count(lookupClass) && classMethodReturnType[lookupClass].count(methodName))
                       ? classMethodReturnType[lookupClass][methodName] : TYPE_UNKNOWN;
    int freshSlot = (mRetType == TYPE_FLOAT) ? alloc_float_slot() : 
                    (mRetType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_temp();
    struct InstructionNode* copyRet = new InstructionNode();
    copyRet->line_no = token.line_no;
    if (mRetType == TYPE_FLOAT)
    {
        copyRet->type = ASSIGN_F;
        copyRet->assign_f_inst.left_hand_side_index = freshSlot;
        copyRet->assign_f_inst.operand1_index = functionReturnIndex[fullName];
        copyRet->assign_f_inst.op = OPERATOR_NONE;
    }
    else if (mRetType == TYPE_DOUBLE)
    {
        copyRet->type = ASSIGN_D;
        copyRet->assign_d_inst.left_hand_side_index = freshSlot;
        copyRet->assign_d_inst.operand1_index = functionReturnIndex[fullName];
        copyRet->assign_d_inst.op = OPERATOR_NONE;
    }
    else
    {
        copyRet->type = ASSIGN;
        copyRet->assign_inst.left_hand_side_index = freshSlot;
        copyRet->assign_inst.operand1_index = functionReturnIndex[fullName];
        copyRet->assign_inst.op = OPERATOR_NONE;
    }
    append(head, tracker, copyRet);
    return freshSlot;
}

int parse_tensor_call(const string& name, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    TensorOp op = TENSOR_OP_MAP.at(name);
    token = lexer.GetToken();  // consume '(' → token is now first arg (or ')')

    // collect arguments
    int argSlots[8]; VarType argTypes[8]; int numArgs = 0;
    while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
    {
        argSlots[numArgs] = parse_expression(head, tracker);
        argTypes[numArgs] = lastExprType;
        numArgs++;
        if (token.token_type == COMMA) token = lexer.GetToken();  // skip ',' -> next arg
    }
    // token is now ')'
    token = lexer.GetToken();  // consume ')'

    int resultSlot = -1;
    bool isVoid = TENSOR_VOID_OPS.count(op) > 0;
    if (!isVoid) { resultSlot = alloc_slot(); }

    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = TENSOR_CALL;
    node->tensor_call_inst.op = op;
    node->tensor_call_inst.result_slot = resultSlot;
    node->tensor_call_inst.num_args = numArgs;
    for (int i = 0; i < numArgs; i++)
    {
        node->tensor_call_inst.arg_slots[i] = argSlots[i];
        node->tensor_call_inst.arg_types[i] = argTypes[i];
    }
    append(head, tracker, node);

    lastExprType = isVoid ? TYPE_UNKNOWN : TYPE_TENSOR;
    return isVoid ? alloc_slot() : resultSlot;
}

void parse_typed_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    VarType declType = TYPE_UNKNOWN;
    if      (token.token_type == INT_TYPE)    declType = TYPE_INT;
    else if (token.token_type == BOOL_TYPE)   declType = TYPE_BOOL;
    else if (token.token_type == STRING_TYPE) declType = TYPE_STRING;
    else if (token.token_type == FLOAT_TYPE) declType = TYPE_FLOAT;
    else if (token.token_type == DOUBLE_TYPE) declType = TYPE_DOUBLE;

    token = lexer.GetToken();
    string name = token.lexeme;
    int nameLine = token.line_no;

    if (symbolTable.count(name) || arrayTable.count(name)) 
    {
        report_error(nameLine, "redeclaration of '" + name + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        return;
    }

    if (declType == TYPE_FLOAT)
    {
        int fSlot = alloc_float_slot();
        floatSymbolTable[name] = fSlot;
        typeTable[name] = TYPE_FLOAT;
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS
        int resultIdx = parse_expression(head, tracker);
        if (lastExprType != TYPE_FLOAT && lastExprType != TYPE_UNKNOWN)
        {
            report_error(nameLine, "cannot assign " + typeToString(lastExprType) + " to float variable '" + name + "'");
        }
        // emit ASSIGN_F
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN_F;
        node->assign_f_inst.left_hand_side_index = fSlot;
        node->assign_f_inst.operand1_index = resultIdx;
        node->assign_f_inst.op = OPERATOR_NONE;
        append(head, tracker, node);
        return;
    }

    if (declType == TYPE_DOUBLE)
    {
        int dSlot = alloc_double_slot();
        doubleSymbolTable[name] = dSlot;
        typeTable[name] = TYPE_DOUBLE;
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS
        int resultIdx = parse_expression(head, tracker);
        if (lastExprType != TYPE_DOUBLE && lastExprType != TYPE_FLOAT && lastExprType != TYPE_UNKNOWN)
        {
            report_error(nameLine, "cannot assign " + typeToString(lastExprType) + " to double variable '" + name + "'");
        }
        // If RHS was a float literal, promote it to double via CAST
        if (lastExprType == TYPE_FLOAT)
        {
            struct InstructionNode* castNode = new InstructionNode();
            castNode->line_no = token.line_no;
            castNode->type = CAST;
            castNode->cast_inst.src_index = resultIdx;
            castNode->cast_inst.dst_index = dSlot;
            castNode->cast_inst.src_type = TYPE_FLOAT;
            castNode->cast_inst.dst_type = TYPE_DOUBLE;
            append(head, tracker, castNode);
            return;
        }
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN_D;
        node->assign_d_inst.left_hand_side_index = dSlot;
        node->assign_d_inst.operand1_index = resultIdx;
        node->assign_d_inst.op = OPERATOR_NONE;
        append(head, tracker, node);
        return;
    }

    symbolTable[name] = alloc_slot();
    typeTable[name] = declType;

    token = lexer.GetToken();  // '='
    token = lexer.GetToken();  // RHS

    if (declType == TYPE_STRING) 
    {
        if (token.token_type == STRING) 
        {
            // plain literal: string s = "hello" ;
            int strIdx = next_str_available++;
            strMem.push_back(token.lexeme);
            int constSlot = alloc_temp();
            mem[constSlot] = strIdx;
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[name];
            node->assign_inst.operand1_index       = constSlot;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            token = lexer.GetToken();  // ';'
        } 
        else 
        {
            // expression: string c = a + b ;
            int resultIdx = parse_expression(head, tracker);
            if (lastExprType != TYPE_STRING && lastExprType != TYPE_UNKNOWN)
                report_error(nameLine, "cannot assign " + typeToString(lastExprType) + " to string variable '" + name + "'");
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[name];
            node->assign_inst.operand1_index       = resultIdx;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            // token is now ';'
        }
        return;
    }

    int resultIdx = parse_expression(head, tracker);
    VarType rhsType = lastExprType;
    // TYPE_TENSOR is allowed on the RHS of int declarations — tensor handles are integers
    bool tensorToInt = (rhsType == TYPE_TENSOR && declType == TYPE_INT);
    if (rhsType != TYPE_UNKNOWN && rhsType != declType && !tensorToInt)
    {
        report_error(nameLine, "cannot assign " + typeToString(rhsType) + " to " + typeToString(declType) + " variable '" + name + "'");
    }
    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[name];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
}

void parse_multi_return_assignment(struct InstructionNode*& head, struct InstructionNode*&tracker)
{
    // ON ENTRY: token = first type keyword
    // Parses: int a, double d = func()
    vector<string> varNames;
    vector<VarType> varTypes;
    vector<int> varSlots;

    while (true)
    {
        VarType vt = TYPE_UNKNOWN;
        if (token.token_type == INT_TYPE) vt = TYPE_INT;
        else if (token.token_type == BOOL_TYPE) vt = TYPE_BOOL;
        else if (token.token_type == STRING_TYPE) vt = TYPE_STRING;
        else if (token.token_type == FLOAT_TYPE) vt = TYPE_FLOAT;
        else if (token.token_type == DOUBLE_TYPE) vt = TYPE_DOUBLE;

        token = lexer.GetToken();  // variable name
        string vname = token.lexeme;
        int vline = token.line_no;

        int vs = (vt == TYPE_FLOAT) ? alloc_float_slot() : 
                 (vt == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
        if (vt == TYPE_FLOAT) floatSymbolTable[vname] = vs;
        else if (vt == TYPE_DOUBLE) doubleSymbolTable[vname] = vs;
        else symbolTable[vname] = vs;
        typeTable[vname] = vt;

        varNames.push_back(vname);
        varTypes.push_back(vt);
        varSlots.push_back(vs);

        token = lexer.GetToken(); // ',' or '='
        if (token.token_type != COMMA) break;
        token = lexer.GetToken();  // next type keyword
    }
    // token = '='
    token = lexer.GetToken();  // function name

    string funcName = token.lexeme;
    int callLine = token.line_no;

    if (!functionTable.count(funcName))
    {
        report_error(callLine, "undeclared function '" + funcName + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return;
    }

    // validate return count
    vector<int>& rSlots = functionReturnSlots[funcName];
    vector<VarType>& rTypes = functionReturnTypesList[funcName];
    if (rSlots.size() != varNames.size())
    {
        report_error(callLine, "function '" + funcName + "' returns " + to_string(rSlots.size()) + " value(s) but " + to_string(varNames.size()) + " variables given");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return;
    }

    // validate types
    for (int i = 0;i < (int)varTypes.size();i++)
    {
        if (rTypes[i] != TYPE_UNKNOWN && varTypes[i] != TYPE_UNKNOWN && rTypes[i] != varTypes[i])
        {
            report_error(callLine, "return value " + to_string(i+1) + " of '" + funcName + "': expected " + typeToString(rTypes[i]) + " but got " + typeToString(varTypes[i])); 
        }
    }

    // emit CALL
    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first arg or ')'
    vector<int> argIndices;
    vector<VarType> argTypes;
    while (token.token_type != RPAREN)
    {
        int ai = parse_expression(head, tracker);
        argIndices.push_back(ai);
        argTypes.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'

    vector<string> params = functionParams[funcName];
    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = token.line_no;
    callNode->type = CALL;
    callNode->call_inst.function_head = functionTable[funcName];
    callNode->call_inst.ret_val_index = rSlots[0];
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;
    vector<int>& fRetSlots = functionReturnSlots[funcName];
    int* allRets = new int[fRetSlots.size()];
    for (int i = 0;i < (int)fRetSlots.size();i++)
    {
        allRets[i] = fRetSlots[i];
    }
    callNode->call_inst.all_ret_slots = allRets;
    callNode->call_inst.num_ret_slots = (int)fRetSlots.size();
    callNode->call_inst.func_slot_base = functionSlotRange.count(funcName) ? functionSlotRange[funcName].first : 0;
    callNode->call_inst.func_slot_count = functionSlotRange.count(funcName) ? functionSlotRange[funcName].second : 0;
    callNode->call_inst.num_params = (int)params.size();
    callNode->call_inst.param_types = nullptr;
    if (!params.empty())
    {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        vector<int>& storedParamSlots = functionParamSlots[funcName];
        for (int i = 0;i < (int)params.size();i++)
        {
            pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        VarType* ptypes = new VarType[params.size()];
        vector<VarType>& pTypes = functionParamTypes[funcName];
        for (int i = 0; i < (int)params.size(); i++)
            ptypes[i] = (i < (int)pTypes.size()) ? pTypes[i] : TYPE_UNKNOWN;
        VarType* atypes = new VarType[params.size()];
        for (int i = 0;i < (int)params.size();i++)
        {
            atypes[i] = (i < (int)argTypes.size()) ? argTypes[i] : TYPE_UNKNOWN;
        }
        callNode->call_inst.param_types = ptypes;
        callNode->call_inst.arg_types = atypes;
        callNode->call_inst.param_slots = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    }
    else
    {
        callNode->call_inst.param_slots = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
        callNode->call_inst.param_types = nullptr;
        callNode->call_inst.arg_types = nullptr;
    }
    append(head, tracker, callNode);

    // copy each return slot -> variable slot
    for (int i = 0;i < (int)varNames.size();i++)
    {
        struct InstructionNode* an = new InstructionNode();
        an->line_no = token.line_no;
        if (varTypes[i] == TYPE_FLOAT)
        {
            an->type = ASSIGN_F;
            an->assign_f_inst.left_hand_side_index = varSlots[i];
            an->assign_f_inst.operand1_index = rSlots[i];
            an->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (varTypes[i] == TYPE_DOUBLE)
        {
            an->type = ASSIGN_D;
            an->assign_d_inst.left_hand_side_index = varSlots[i];
            an->assign_d_inst.operand1_index = rSlots[i];
            an->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            an->type = ASSIGN;
            an->assign_inst.left_hand_side_index = varSlots[i];
            an->assign_inst.operand1_index = rSlots[i];
            an->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, an);
    }
    // token = ';'
}

void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == INT_TYPE || token.token_type == BOOL_TYPE || 
        token.token_type == STRING_TYPE || token.token_type == FLOAT_TYPE || 
        token.token_type == DOUBLE_TYPE) 
    {
        // multi-return: TYPE ID, TYPE ID = func() ;    
        if (lexer.peek(2).token_type == COMMA)
        {
            parse_multi_return_assignment(head, tracker);
        }
        else
        {
            parse_typed_declaration(head, tracker);
        }
    } 
    else if (token.token_type == ID && TENSOR_OP_MAP.count(token.lexeme) && TENSOR_VOID_OPS.count(TENSOR_OP_MAP.at(token.lexeme)))
    {
        // void tensor call as a statement: tensor_print(t) ; or tensor)set(t, i, j, v) ;
        string name = token.lexeme;
        token = lexer.GetToken();  // move past name to '('
        parse_tensor_call(name, head, tracker);
        // token is now ';' — outer loop will advance past it
    }
    else if (token.token_type == PRINT) 
    {
        parse_output_statement(head, tracker);
    } 
    else if (token.token_type == IF) 
    {
        parse_if_statement(head, tracker);
    } 
    else if (token.token_type == WHILE) 
    {
        parse_while_statement(head, tracker);
    } 
    else if (token.token_type == DO)
    {
        parse_do_while_statement(head, tracker);
    }
    else if (token.token_type == FOR) 
    {
        parse_for_statement(head, tracker);
    } 
    else if (token.token_type == SWITCH) 
    {
        parse_switch_statement(head, tracker);
    } 
    else if (token.token_type == RETURN) 
    {
        if (!insideFunction) 
        {
            report_error(token.line_no, "return statement outside of a function");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        } 
        else 
        {
            token = lexer.GetToken();
            vector<int> retExprs;
            retExprs.push_back(parse_expression(head, tracker));
            while (token.token_type == COMMA)
            {
                token = lexer.GetToken();
                retExprs.push_back(parse_expression(head, tracker));
            }
            int nExpected = currentFuncRetSlots.empty() ? 1 : (int)currentFuncRetSlots.size();
            if ((int)retExprs.size() != nExpected)
            {
                report_error(token.line_no, "return count mismatch: expected " + to_string(nExpected) + " got " + to_string(retExprs.size()));
            }
            for (int i = 0;i < (int)retExprs.size();i++)
            {
                int rSlot = (i < (int)currentFuncRetSlots.size()) ? currentFuncRetSlots[i] : currentFuncRetIdx;
                VarType rt = (i < (int)currentFuncRetTypes.size()) ? currentFuncRetTypes[i] : currentFuncRetType;
                struct InstructionNode* an = new InstructionNode();
                an->line_no = token.line_no;
                if (rt == TYPE_FLOAT)
                {
                    an->type = ASSIGN_F;
                    an->assign_f_inst.left_hand_side_index = rSlot;
                    an->assign_f_inst.operand1_index = retExprs[i];
                    an->assign_f_inst.op = OPERATOR_NONE;   
                }
                else if (rt == TYPE_DOUBLE)
                {
                    an->type = ASSIGN_D;
                    an->assign_d_inst.left_hand_side_index = rSlot;
                    an->assign_d_inst.operand1_index = retExprs[i];
                    an->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    an->type = ASSIGN;
                    an->assign_inst.left_hand_side_index = rSlot;
                    an->assign_inst.operand1_index = retExprs[i];
                    an->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, an);
            }
            struct InstructionNode* retNode = new InstructionNode();
            retNode->line_no = token.line_no;
            retNode->type = RET;
            retNode->ret_inst.ret_val_index = currentFuncRetIdx;
            append(head, tracker, retNode);
        }
    } 
    else if (token.token_type == SELF)
    {
        // self.field = expr ; inside a class method
        if (!insideFunction || currentSelfClassName.empty())
        {
            report_error(token.line_no, "'self' used outside of a class method");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
        token = lexer.GetToken();  // consume SELF → '.'
        string fieldPath = "";
        while (token.token_type == DOT)
        {
            token = lexer.GetToken();  // consume '.' → field segment
            if (!fieldPath.empty()) fieldPath += ".";
            fieldPath += token.lexeme;
            token = lexer.GetToken();  // consume field segment → next token
        }
        // self.field[index] = value (array write)
        if (token.token_type == LBRAC)
        {
            if (!currentSelfFieldSlots.count(fieldPath))
            {
                report_error(token.line_no, "'" + currentSelfClassName + "' has no array field '" + fieldPath + "'");
                while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
                {
                    token = lexer.GetToken();
                }
                return;
            }
            token = lexer.GetToken();  // first token of index expression
            int indexSlot = parse_expression(head, tracker);
            token = lexer.GetToken();  // past ']' -> '='
            token = lexer.GetToken();  // past '=' -> value expression
            int valueSlot = parse_expression(head, tracker);

            int arrSize = currentSelfFieldArraySizes.count(fieldPath) ? currentSelfFieldArraySizes[fieldPath] : 0;

            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ARRAY_WRITE;
            node->array_inst.base_index = currentSelfFieldSlots[fieldPath];
            node->array_inst.dynamic_base = true;
            node->array_inst.index_slot = indexSlot;
            node->array_inst.target_index = valueSlot;
            node->array_inst.array_size = arrSize;
            node->array_inst.size_slot = -1;
            node->array_inst.line_no = token.line_no;
            append(head, tracker, node);
            return;
        }
        // token = '='
        token = lexer.GetToken();  // consume '=' → RHS
        int resultIdx = parse_expression(head, tracker);

        if (!currentSelfFieldSlots.count(fieldPath))
        {
            report_error(token.line_no, "class '" + currentSelfClassName + "' has no field '" + fieldPath + "'");
            return;
        }
        int slot = currentSelfFieldSlots[fieldPath];
        VarType fType = currentSelfFieldTypes[fieldPath];
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        if (fType == TYPE_FLOAT)
        {
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = slot;
            node->assign_f_inst.operand1_index = resultIdx;
            node->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fType == TYPE_DOUBLE)
        {
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = slot;
            node->assign_d_inst.operand1_index = resultIdx;
            node->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = slot;
            node->assign_inst.operand1_index = resultIdx;
            node->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, node);
    }
    else if (token.token_type == ID)
    {
        string name = token.lexeme;
        if (classTable.count(name) && lexer.peek(1).token_type == ID)
        {
            // class instantiation: MyClass obj ; or MyClass obj = { ... } ;
            parse_class_instantiation(name, head, tracker);
        }
        else if (varClassType.count(name) && lexer.peek(1).token_type == DOT)
        {
            // obj.method(args) ; or obj.field = expr ;
            int objLine = token.line_no;
            token = lexer.GetToken();  // consume obj name → '.'
            token = lexer.GetToken();  // consume '.' → member name
            string memberName = token.lexeme;
            token = lexer.GetToken();  // consume member name → '(' or '='

            if (token.token_type == LPAREN)
            {
                parse_method_call(name, memberName, head, tracker);
                // token is now ';'
            }
            else
            {
                // field assignment
                if (token.token_type != EQUAL)
                {
                    report_error(objLine, "expected '=' or '(' after '" + name + "." + memberName + "'");
                    while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
                    return;
                }
                token = lexer.GetToken();  // consume '=' → RHS
                int resultIdx = parse_expression(head, tracker);

                if (!classFieldSlots.count(name) || !classFieldSlots[name].count(memberName))
                {
                    report_error(objLine, "class instance '" + name + "' has no field '" + memberName + "'");
                    return;
                }
                int slot = classFieldSlots[name][memberName];
                VarType fType = classFieldTypes[name][memberName];
                struct InstructionNode* node = new InstructionNode();
                node->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    node->type = ASSIGN_F;
                    node->assign_f_inst.left_hand_side_index = slot;
                    node->assign_f_inst.operand1_index = resultIdx;
                    node->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    node->type = ASSIGN_D;
                    node->assign_d_inst.left_hand_side_index = slot;
                    node->assign_d_inst.operand1_index = resultIdx;
                    node->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    node->type = ASSIGN;
                    node->assign_inst.left_hand_side_index = slot;
                    node->assign_inst.operand1_index = resultIdx;
                    node->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, node);
            }
        }
        else if (structTable.count(name) && lexer.peek(1).token_type == ID)
        {
            // struct instantiation: Point p ; or Point p = {3.14, 2.71} ;
            parse_struct_instantiation(name, head, tracker);
        }
        else if (arrayTable.find(name) != arrayTable.end())
        {
            parse_assignment_statement(head, tracker);
        }
        else if (functionTable.find(name) != functionTable.end() && lexer.peek(1).token_type == LPAREN)
        {
            parse_function_call(head, tracker);
        }
        else
        {
            parse_assignment_statement(head, tracker);
        }
    }
    else if (token.token_type != RBRACE)
    {
        report_error(token.line_no, "unexpected token '" + token.lexeme + "'");
    }
}

void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* newNode = new InstructionNode();
    newNode->line_no = token.line_no;
    newNode->type = OUT;
    newNode->output_inst.newline       = true;
    newNode->output_inst.is_string     = false;
    newNode->output_inst.is_string_var = false;
    newNode->output_inst.value_type = TYPE_INT;  // default

    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // content

    if (token.token_type == STRING) {
        newNode->output_inst.var_index = next_str_available++;
        newNode->output_inst.is_string = true;
        newNode->output_inst.is_string_var = false;
        newNode->output_inst.value_type = TYPE_STRING;
        strMem.push_back(token.lexeme);
        token = lexer.GetToken();
    } 
    else 
    {
        int resultIdx = parse_expression(head, tracker);
        VarType exprType = lastExprType;
        newNode->output_inst.var_index = resultIdx;
        newNode->output_inst.value_type = exprType;
        if (exprType == TYPE_STRING) 
        {
            newNode->output_inst.is_string     = true;
            newNode->output_inst.is_string_var = true;
        }
    }

    token = lexer.GetToken();  // ';'
    append(head, tracker, newNode);
}

int parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string funcName = token.lexeme;
    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first arg or ')'

    vector<int>     argIndices;
    vector<VarType> argTypes;

    while (token.token_type != RPAREN) {
        int argIdx = parse_expression(head, tracker);
        argIndices.push_back(argIdx);
        argTypes.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'

    vector<string>& params = functionParams[funcName];
    if (argIndices.size() != params.size())
        report_error(token.line_no, "function '" + funcName + "' expects " + to_string(params.size()) + " argument(s) but got " + to_string(argIndices.size()));

    if (functionParamTypes.count(funcName)) {
        vector<VarType>& pTypes = functionParamTypes[funcName];
        for (int i = 0; i < (int)argTypes.size() && i < (int)pTypes.size(); i++) {
            if (argTypes[i] != TYPE_UNKNOWN && pTypes[i] != TYPE_UNKNOWN && argTypes[i] != pTypes[i])
                report_error(token.line_no, "argument " + to_string(i+1) + " of '" + funcName + "' expects " + typeToString(pTypes[i]) + " but got " + typeToString(argTypes[i]));
        }
    }

    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = token.line_no;
    callNode->type = CALL;
    callNode->call_inst.function_head   = functionTable[funcName];
    callNode->call_inst.ret_val_index   = functionReturnIndex[funcName];
    callNode->call_inst.func_slot_base  = functionSlotRange.count(funcName) ? functionSlotRange[funcName].first : 0;
    callNode->call_inst.func_slot_count = functionSlotRange.count(funcName) ? functionSlotRange[funcName].second : 0;
    callNode->call_inst.num_params      = (int)params.size();
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;

    if (params.size() > 0) {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        vector<int>& storedParamSlots = functionParamSlots[funcName];
        for (int i = 0; i < (int)params.size(); i++) {
            pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        VarType* pTypes = new VarType[params.size()];
        vector<VarType>& storedParamTypes = functionParamTypes[funcName];
        for (int i = 0;i < (int)params.size();i++)
        {
            pTypes[i] = (i < (int)storedParamTypes.size()) ? storedParamTypes[i] : TYPE_UNKNOWN;
        }
        VarType* aTypes = new VarType[params.size()];
        for (int i = 0;i < (int)params.size();i++)
        {
            aTypes[i] = (i < (int)argTypes.size()) ? argTypes[i] : TYPE_UNKNOWN;
        }
        callNode->call_inst.param_types = pTypes;
        callNode->call_inst.arg_types = aTypes;
        callNode->call_inst.param_slots   = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    } 
    else {
        callNode->call_inst.param_slots   = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
        callNode->call_inst.param_types = nullptr;
        callNode->call_inst.arg_types = nullptr;
    }
    append(head, tracker, callNode);

    int freshSlot = alloc_temp();
    struct InstructionNode* copyRetNode = new InstructionNode();
    copyRetNode->line_no = token.line_no;
    copyRetNode->type = ASSIGN;
    copyRetNode->assign_inst.left_hand_side_index = freshSlot;
    copyRetNode->assign_inst.operand1_index       = functionReturnIndex[funcName];
    copyRetNode->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, copyRetNode);

    lastExprType = functionReturnType.count(funcName) ? functionReturnType[funcName] : TYPE_UNKNOWN;
    return freshSlot;
}

int parse_factor(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == LPAREN) 
    {
        token = lexer.GetToken();
        int result = parse_expression(head, tracker);
        token = lexer.GetToken();
        return result;
    } 
    else if (token.token_type == NUM) 
    {
        int idx = alloc_slot();
        mem[idx] = stoi(token.lexeme);
        lastExprType = TYPE_INT;
        token = lexer.GetToken();
        return idx;
    } 
    else if (token.token_type == TRUE) 
    {
        int idx = alloc_slot();
        mem[idx] = 1;
        lastExprType = TYPE_BOOL;
        token = lexer.GetToken();
        return idx;
    } 
    else if (token.token_type == FALSE) 
    {
        int idx = alloc_slot();
        mem[idx] = 0;
        lastExprType = TYPE_BOOL;
        token = lexer.GetToken();
        return idx;
    } 
    else if (token.token_type == INPUT) 
    {
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // prompt or ')'
        if (token.token_type == STRING) token = lexer.GetToken();
        token = lexer.GetToken();  // past ')'
        int slot = alloc_temp();
        struct InstructionNode* inNode = new InstructionNode();
        inNode->line_no = token.line_no;
        inNode->type = IN;
        inNode->input_inst.var_index = slot;
        append(head, tracker, inNode);
        lastExprType = TYPE_INT;
        return slot;
    } 
    else if (token.token_type == ID) 
    {
        if (functionTable.find(token.lexeme) != functionTable.end()) return parse_function_call(head, tracker);

        if (TENSOR_OP_MAP.count(token.lexeme))
        {
            string name = token.lexeme;
            token = lexer.GetToken();  // move past the name to '('
            return parse_tensor_call(name, head, tracker);
        }

        if (arrayTable.find(token.lexeme) != arrayTable.end()) 
        {
            string arrName = token.lexeme;
            int    base    = arrayTable[arrName];
            token = lexer.GetToken();  // '['
            token = lexer.GetToken();
            int indexLine = token.line_no;
            if (token.token_type == NUM) check_bounds(arrName, stoi(token.lexeme), indexLine);
            int indexSlot = parse_expression(head, tracker);
            token = lexer.GetToken();  // past ']'
            int tempSlot = alloc_temp();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ARRAY_READ;
            bool isDyn = arrayDynamic.count(arrName) && arrayDynamic[arrName];
            node->array_inst.base_index   = base;
            node->array_inst.dynamic_base = isDyn;
            node->array_inst.index_slot   = indexSlot;
            node->array_inst.target_index = tempSlot;
            node->array_inst.array_size   = isDyn ? -1 : arraySizeTable[arrName];
            node->array_inst.size_slot = isDyn ? arraySizeTable[arrName] : -1;
            node->array_inst.line_no = indexLine;
            append(head, tracker, node);
            lastExprType = TYPE_INT;
            return tempSlot;
        }

        if (typeTable.count(token.lexeme) && typeTable[token.lexeme] == TYPE_FLOAT)
        {
            lastExprType = TYPE_FLOAT;
            int idx = floatSymbolTable[token.lexeme];
            token = lexer.GetToken();
            return idx;
        }

        if (typeTable.count(token.lexeme) && typeTable[token.lexeme] == TYPE_DOUBLE)
        {
            lastExprType = TYPE_DOUBLE;
            int idx = doubleSymbolTable[token.lexeme];
            token = lexer.GetToken();
            return idx;
        }

        // SELF keyword: self.field read inside a class method
        // (handled as a separate else-if branch outside this ID block;
        //  but if someone aliases "self" as an ID, handle gracefully)

        // class instance field read or method call: obj.field or obj.method(args)
        if (varClassType.count(token.lexeme) && lexer.peek(1).token_type == DOT)
        {
            string varName = token.lexeme;
            token = lexer.GetToken();  // consume var name → '.'
            token = lexer.GetToken();  // consume '.' → member name
            string memberName = token.lexeme;
            token = lexer.GetToken();  // consume member name → next token

            if (token.token_type == LPAREN)
            {
                // method call in expression context: obj.method(args)
                return parse_method_call(varName, memberName, head, tracker);
            }
            else
            {
                // field read: obj.field
                if (!classFieldSlots.count(varName) || !classFieldSlots[varName].count(memberName))
                {
                    report_error(token.line_no, "class instance '" + varName + "' has no field '" + memberName + "'");
                    lastExprType = TYPE_UNKNOWN;
                    return 0;
                }
                lastExprType = classFieldTypes[varName][memberName];
                return classFieldSlots[varName][memberName];
            }
        }

        // struct field read: p.x or L.a.x
        if (varStructType.count(token.lexeme) && lexer.peek(1).token_type == DOT)
        {
            string varName = token.lexeme;
            token = lexer.GetToken();  // consume var name -> token = DOT

            string fieldPath = "";
            while (token.token_type == DOT)
            {
                token = lexer.GetToken();  // consume DOT -> token = field segment
                if (!fieldPath.empty()) fieldPath += ".";
                fieldPath += token.lexeme;
                token = lexer.GetToken();  // consume field segment -> next token
            }
            // token is now whatever follows the field access

            if (!structFieldSlots[varName].count(fieldPath))
            {
                report_error(token.line_no, "struct '" + varName + "' has no field '" + fieldPath + "'");
                lastExprType = TYPE_UNKNOWN;
                return 0;
            }
            lastExprType = structFieldTypes[varName][fieldPath];
            return structFieldSlots[varName][fieldPath];
        }

        if (!check_declared(token.lexeme, token.line_no)) { token = lexer.GetToken(); lastExprType = TYPE_UNKNOWN; return 0; }

        lastExprType = typeTable.count(token.lexeme) ? typeTable[token.lexeme] : TYPE_UNKNOWN;
        int idx = symbolTable[token.lexeme];
        token = lexer.GetToken();
        return idx;
    }
    else if (token.token_type == STRING)
    {
        int strIdx = next_str_available++;
        strMem.push_back(token.lexeme);
        int slot = alloc_slot();
        mem[slot] = strIdx;
        lastExprType = TYPE_STRING;
        token = lexer.GetToken();
        return slot;
    }
    else if (token.token_type == FLOAT_LITERAL)
    {
        // store in fmem, return from index
        int idx = alloc_float_slot();
        fmem[idx] = stof(token.lexeme);
        lastExprType = TYPE_FLOAT;
        token = lexer.GetToken();
        return idx;
    }
    else if (token.token_type == SELF)
    {
        // self.field read inside a class method
        if (!insideFunction || currentSelfClassName.empty())
        {
            report_error(token.line_no, "'self' used outside of a class method");
            lastExprType = TYPE_UNKNOWN;
            return 0;
        }
        token = lexer.GetToken();  // consume SELF → '.'
        string fieldPath = "";
        while (token.token_type == DOT)
        {
            token = lexer.GetToken();  // consume '.' → field segment
            if (!fieldPath.empty()) fieldPath += ".";
            fieldPath += token.lexeme;
            token = lexer.GetToken();  // consume field segment → next token
        }
        // self.field[index] (array read)
        if (token.token_type == LBRAC)
        {
            if (!currentSelfFieldSlots.count(fieldPath))
            {
                report_error(token.line_no, "'" + currentSelfClassName + "' has no array field '" + fieldPath + "'");
                lastExprType = TYPE_UNKNOWN;
                return 0;
            }
            int indexLine = token.line_no;
            token = lexer.GetToken();  // first token of index expression
            int indexSlot = parse_expression(head, tracker);
            token = lexer.GetToken();  // past ']'

            int arrSize = currentSelfFieldArraySizes.count(fieldPath) ? currentSelfFieldArraySizes[fieldPath] : 0;

            int tempSlot = alloc_temp();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ARRAY_READ;
            node->array_inst.base_index = currentSelfFieldSlots[fieldPath];
            node->array_inst.dynamic_base = true;
            node->array_inst.index_slot = indexSlot;
            node->array_inst.target_index = tempSlot;
            node->array_inst.array_size = arrSize;
            node->array_inst.size_slot = -1;
            node->array_inst.line_no = indexLine;
            append(head, tracker, node);

            lastExprType = TYPE_INT;
            return tempSlot;
        }
        if (!currentSelfFieldSlots.count(fieldPath))
        {
            report_error(token.line_no, "class '" + currentSelfClassName + "' has no field '" + fieldPath + "'");
            lastExprType = TYPE_UNKNOWN;
            return 0;
        }
        lastExprType = currentSelfFieldTypes[fieldPath];
        return currentSelfFieldSlots[fieldPath];
    }
    else if (token.token_type == FLOAT_TYPE || token.token_type == DOUBLE_TYPE || token.token_type == INT_TYPE)
    {
        // explicit cast: float(x), double(x), int(x)
        VarType dstType = (token.token_type == FLOAT_TYPE) ? TYPE_FLOAT :
                          (token.token_type == DOUBLE_TYPE) ? TYPE_DOUBLE : TYPE_INT;
        token = lexer.GetToken(); // '('
        token = lexer.GetToken(); // expression start
        int srcIdx = parse_expression(head, tracker);
        VarType srcType = lastExprType;
        token = lexer.GetToken(); // past ')'

        // allocate destination slot 
        int dstIdx = 0;
        if (dstType == TYPE_FLOAT) dstIdx = alloc_float_slot();
        else if (dstType == TYPE_DOUBLE) dstIdx = alloc_double_slot();
        else dstIdx = alloc_slot();

        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = CAST;
        node->cast_inst.src_index = srcIdx;
        node->cast_inst.dst_index = dstIdx;
        node->cast_inst.src_type = srcType;
        node->cast_inst.dst_type = dstType;
        append(head, tracker, node);

        lastExprType = dstType;
        return dstIdx;
    }
    lastExprType = TYPE_UNKNOWN;
    return 0;
}

int parse_term(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_factor(head, tracker);
    VarType leftType = lastExprType;
    while (token.token_type == MULT || token.token_type == DIV) {
        int opLine = token.line_no;
        ArithmeticOperatorType op = (token.token_type == MULT) ? OPERATOR_MULT : OPERATOR_DIV;

        if (leftType == TYPE_FLOAT)
        {
            token = lexer.GetToken();
            int right = parse_factor(head, tracker);
            int tmp = alloc_float_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = tmp;
            node->assign_f_inst.operand1_index = left;
            node->assign_f_inst.operand2_index = right;
            node->assign_f_inst.op = op;
            append(head, tracker, node);
            left = tmp; leftType = TYPE_FLOAT; lastExprType = TYPE_FLOAT;
            continue;
        }

        if (leftType == TYPE_DOUBLE)
        {
            token = lexer.GetToken();
            int right = parse_factor(head, tracker);
            int tmp = alloc_double_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = tmp;
            node->assign_d_inst.operand1_index = left;
            node->assign_d_inst.operand2_index = right;
            node->assign_d_inst.op = op;
            append(head, tracker, node);
            left = tmp; leftType = TYPE_DOUBLE; lastExprType = TYPE_DOUBLE;
            continue;
        }

        if (leftType != TYPE_UNKNOWN && leftType != TYPE_INT) report_error(opLine, "arithmetic requires int operands, got " + typeToString(leftType));
        token = lexer.GetToken();
        int right = parse_factor(head, tracker);
        VarType rightType = lastExprType;

        if (rightType != TYPE_UNKNOWN && rightType != TYPE_INT) report_error(opLine, "arithmetic requires int operands, got " + typeToString(rightType));
        int tmp = alloc_temp();

        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN;
        node->assign_inst.left_hand_side_index = tmp;
        node->assign_inst.operand1_index       = left;
        node->assign_inst.operand2_index       = right;
        node->assign_inst.op                   = op;
        append(head, tracker, node);
        left = tmp; leftType = TYPE_INT; lastExprType = TYPE_INT;
    }
    return left;
}

int parse_expression(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_term(head, tracker);
    VarType leftType = lastExprType;
    while (token.token_type == PLUS || token.token_type == MINUS) 
    {
        int opLine = token.line_no;
        ArithmeticOperatorType op = (token.token_type == PLUS) ? OPERATOR_PLUS : OPERATOR_MINUS;

        if (op == OPERATOR_PLUS && leftType == TYPE_STRING)
        {
            token = lexer.GetToken();
            int right = parse_term(head, tracker);
            if (lastExprType != TYPE_STRING && lastExprType != TYPE_UNKNOWN)
            {
                report_error(opLine, "cannot concatenate string with " + typeToString(lastExprType));
            }
            int dest = alloc_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = STRCAT;
            node->strcat_inst.dest_slot = dest;
            node->strcat_inst.left_slot = left;
            node->strcat_inst.right_slot = right;
            append(head, tracker, node);
            left = dest; 
            leftType = TYPE_STRING;
            lastExprType = TYPE_STRING;
            continue;
        }

        if (leftType == TYPE_FLOAT)
        {
            token = lexer.GetToken();
            int right = parse_term(head, tracker);
            int tmp = alloc_float_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = tmp;
            node->assign_f_inst.operand1_index       = left;
            node->assign_f_inst.operand2_index       = right;
            node->assign_f_inst.op                   = op;
            append(head, tracker, node);
            left = tmp; leftType = TYPE_FLOAT; lastExprType = TYPE_FLOAT;
            continue;
        }

        if (leftType == TYPE_DOUBLE)
        {
            token = lexer.GetToken();
            int right = parse_term(head, tracker);
            int tmp = alloc_double_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = tmp;
            node->assign_d_inst.operand1_index       = left;
            node->assign_d_inst.operand2_index       = right;
            node->assign_d_inst.op                   = op;
            append(head, tracker, node);
            left = tmp; leftType = TYPE_DOUBLE; lastExprType = TYPE_DOUBLE;
            continue;
        }

        if (leftType != TYPE_UNKNOWN && leftType != TYPE_INT)
        {
            report_error(opLine, "arithmetic requires int operands, got " + typeToString(leftType));
        }
        token = lexer.GetToken();
        int right = parse_term(head, tracker);
        VarType rightType = lastExprType;
        if (rightType != TYPE_UNKNOWN && rightType != TYPE_INT) 
        {
            report_error(opLine, "arithmetic requires int operands, got " + typeToString(rightType));
        }
        int tmp = alloc_temp();
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN;
        node->assign_inst.left_hand_side_index = tmp;
        node->assign_inst.operand1_index       = left;
        node->assign_inst.operand2_index       = right;
        node->assign_inst.op                   = op;
        append(head, tracker, node);
        left = tmp; leftType = TYPE_INT; lastExprType = TYPE_INT;
    }
    return left;
}

void parse_assignment_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string lhs = token.lexeme;
    int lhs_line = token.line_no;

    // struct field write: p.x = 3.14 ; or L.a.x = 1.0 ;
    if (varStructType.count(lhs) && lexer.peek(1).token_type == DOT)
    {
        token = lexer.GetToken();  // consume var name -> DOT

        string fieldPath = "";
        while (token.token_type == DOT)
        {
            token = lexer.GetToken();  // consume DOT → field segment
            if (!fieldPath.empty()) fieldPath += ".";
            fieldPath += token.lexeme;
            token = lexer.GetToken();  // consume field segment → next token
        }

        // token = '='
        token = lexer.GetToken();  // consume '=' → RHS start

        int resultIdx = parse_expression(head, tracker);
        // token = ';'

        if (!structFieldSlots[lhs].count(fieldPath))
        {
            report_error(lhs_line, "struct '" + lhs + "' has no field '" + fieldPath + "'");
            return;
        }

        int slot = structFieldSlots[lhs][fieldPath];
        VarType fieldType = structFieldTypes[lhs][fieldPath];

        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        if (fieldType == TYPE_FLOAT)
        {
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = slot;
            node->assign_f_inst.operand1_index = resultIdx;
            node->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fieldType == TYPE_DOUBLE)
        {
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = slot;
            node->assign_d_inst.operand1_index = resultIdx;
            node->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = slot;
            node->assign_inst.operand1_index = resultIdx;
            node->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, node);
        return;
    }

    if (arrayTable.find(lhs) != arrayTable.end()) {
        token = lexer.GetToken();  // '['
        token = lexer.GetToken();
        int indexLine = token.line_no;
        if (token.token_type == NUM && !(arrayDynamic.count(lhs) && arrayDynamic[lhs]))
        {
            check_bounds(lhs, stoi(token.lexeme), indexLine);
        }
        int indexSlot = parse_expression(head, tracker);
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();
        int valueSlot = parse_expression(head, tracker);
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ARRAY_WRITE;
        bool isDyn = arrayDynamic.count(lhs) && arrayDynamic[lhs];
        node->array_inst.base_index   = arrayTable[lhs];
        node->array_inst.dynamic_base = isDyn;
        node->array_inst.index_slot   = indexSlot;
        node->array_inst.target_index = valueSlot;
        node->array_inst.array_size   = isDyn ? -1 : arraySizeTable[lhs];
        node->array_inst.size_slot = isDyn ? arraySizeTable[lhs] : -1;
        node->array_inst.line_no = lhs_line;
        append(head, tracker, node);
        return;
    }

    if (symbolTable.find(lhs) == symbolTable.end() && lexer.peek(1).token_type == EQUAL && lexer.peek(2).token_type == ARRAY) {
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // 'array'
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // first token of size expression

        if (token.token_type == NUM && lexer.peek(1).token_type == RPAREN) 
        {
            // ── static: array(5) ─────────────────────────────────────────────────
            int size = stoi(token.lexeme);
            if (size <= 0) { report_error(lhs_line, "array '" + lhs + "' must have size > 0"); return; }
            token = lexer.GetToken();  // ')'
            token = lexer.GetToken();  // ';'
            arrayTable[lhs]    = next_available;
            arraySizeTable[lhs] = size;
            arrayDynamic[lhs]   = false;
            for (int i = 0; i < size; i++) alloc_slot();
        } 
        else 
        {
            // ── dynamic: array(n) or array(n * 2) ────────────────────────────────
            int sizeSlot = parse_expression(head, tracker);
            // token is now ')'
            token = lexer.GetToken();  // ';'

            int baseSlot       = alloc_slot();  // holds base index at runtime
            int storedSizeSlot = alloc_slot();  // holds size at runtime for bounds checks

            // copy computed size into storedSizeSlot
            struct InstructionNode* copySize = new InstructionNode();
            copySize->line_no = token.line_no;
            copySize->type = ASSIGN;
            copySize->assign_inst.left_hand_side_index = storedSizeSlot;
            copySize->assign_inst.operand1_index       = sizeSlot;
            copySize->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, copySize);

            // emit ALLOC — fills baseSlot at runtime
            struct InstructionNode* allocNode = new InstructionNode();
            allocNode->line_no = token.line_no;
            allocNode->type = ALLOC;
            allocNode->alloc_inst.base_slot = baseSlot;
            allocNode->alloc_inst.size_slot = storedSizeSlot;
            append(head, tracker, allocNode);

            arrayTable[lhs]    = baseSlot;
            arraySizeTable[lhs] = storedSizeSlot;
            arrayDynamic[lhs]   = true;
        }
        return;
    }

    if (symbolTable.count(lhs) && typeTable.count(lhs) && typeTable[lhs] == TYPE_STRING) {
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS start
        if (token.token_type == STRING) 
        {
            // plain literal: a = "hello" ;
            int strIdx = next_str_available++;
            strMem.push_back(token.lexeme);
            int constSlot = alloc_temp();
            mem[constSlot] = strIdx;
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[lhs];
            node->assign_inst.operand1_index       = constSlot;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            token = lexer.GetToken();  // ';'
        } 
        else 
        {
            // expression: a = b + c ;
            int resultIdx = parse_expression(head, tracker);
            if (lastExprType != TYPE_STRING && lastExprType != TYPE_UNKNOWN)
                report_error(lhs_line, "cannot assign " + typeToString(lastExprType) + " to string variable '" + lhs + "'");
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[lhs];
            node->assign_inst.operand1_index       = resultIdx;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            // token is now ';'
        }
        return;
    }

    if (symbolTable.find(lhs) == symbolTable.end()) {
        symbolTable[lhs] = alloc_slot(); typeTable[lhs] = TYPE_UNKNOWN;
    }

    token = lexer.GetToken(); token = lexer.GetToken();
    int resultIdx = parse_expression(head, tracker);
    VarType rhsType = lastExprType;
    VarType lhsType = typeTable.count(lhs) ? typeTable[lhs] : TYPE_UNKNOWN;
    bool tensorToInt = (rhsType == TYPE_TENSOR && lhsType == TYPE_INT);
    if (lhsType != TYPE_UNKNOWN && rhsType != TYPE_UNKNOWN && lhsType != rhsType && !tensorToInt)
        report_error(lhs_line, "cannot assign " + typeToString(rhsType) + " to " + typeToString(lhsType) + " variable '" + lhs + "'");

    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[lhs];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
}

void parse_assignment_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string lhs = token.lexeme;
    token = lexer.GetToken(); token = lexer.GetToken();
    int resultIdx = parse_expression(head, tracker);
    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[lhs];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
}

void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* noOpNode)
{
    auto set_target = [](struct InstructionNode* node, struct InstructionNode* target) 
    {
        if (node->type == SCMP) node->scmp_inst.target = target;
        else                    node->cjmp_inst.target = target;
    };
    if (token.token_type == NOT) 
    {
        token = lexer.GetToken();
        int lhsIdx = parse_expression(head, tracker);
        VarType lhsCondType = lastExprType;

        ConditionalOperatorType condOp = CONDITION_NOTEQUAL;
        int rhsIdx;
        if (token.token_type == GREATER || token.token_type == LESS || token.token_type == NOTEQUAL)
        {
            if      (token.token_type == GREATER)  condOp = CONDITION_GREATER;
            else if (token.token_type == LESS)     condOp = CONDITION_LESS;
            else                                   condOp = CONDITION_NOTEQUAL;
            token = lexer.GetToken();
            rhsIdx = parse_expression(head, tracker);
        }
        else
        {
            int zeroSlot = alloc_slot();
            mem[zeroSlot] = 0;
            rhsIdx = zeroSlot;
        }
        VarType rhsCondType = lastExprType;

        bool isStringCmp = (lhsCondType == TYPE_STRING || rhsCondType == TYPE_STRING);

        struct InstructionNode* cjmpNode = new InstructionNode();
        cjmpNode->line_no = token.line_no;
        if (isStringCmp) 
        {
            cjmpNode->type = SCMP;
            cjmpNode->scmp_inst.condition_op   = condOp;
            cjmpNode->scmp_inst.operand1_index = lhsIdx;
            cjmpNode->scmp_inst.operand2_index = rhsIdx;
        } 
        else 
        {
            cjmpNode->type = CJMP;
            cjmpNode->cjmp_inst.condition_op   = condOp;
            cjmpNode->cjmp_inst.operand1_index = lhsIdx;
            cjmpNode->cjmp_inst.operand2_index = rhsIdx;
        }

        struct InstructionNode* bodyEntry = new InstructionNode();
        bodyEntry->line_no = token.line_no;
        bodyEntry->type = NOOP;
        cjmpNode->cjmp_inst.target = bodyEntry;
        append(head, tracker, cjmpNode);
        struct InstructionNode* skipNode = new InstructionNode();
        skipNode->line_no = token.line_no;
        skipNode->type = JMP; 
        skipNode->jmp_inst.target = noOpNode;
        append(head, tracker, skipNode);
        append(head, tracker, bodyEntry);
        tracker = bodyEntry;
        return;
    }

    int lhsIdx = parse_expression(head, tracker);
    VarType lhsCondType = lastExprType;

    ConditionalOperatorType condOp = CONDITION_NOTEQUAL;
    if      (token.token_type == GREATER)  condOp = CONDITION_GREATER;
    else if (token.token_type == LESS)     condOp = CONDITION_LESS;
    else if (token.token_type == NOTEQUAL) condOp = CONDITION_NOTEQUAL;
    token = lexer.GetToken();

    int rhsIdx = parse_expression(head, tracker);
    VarType rhsCondType = lastExprType;

    bool isStringCmp = (lhsCondType == TYPE_STRING || rhsCondType == TYPE_STRING);

    struct InstructionNode* cjmpNode = new InstructionNode();
    cjmpNode->line_no = token.line_no;
    if (isStringCmp)
    {
        cjmpNode->type = SCMP;
        cjmpNode->scmp_inst.condition_op   = condOp;
        cjmpNode->scmp_inst.operand1_index = lhsIdx;
        cjmpNode->scmp_inst.operand2_index = rhsIdx;
    } 
    else 
    {
        cjmpNode->type = CJMP;
        cjmpNode->cjmp_inst.condition_op   = condOp;
        cjmpNode->cjmp_inst.operand1_index = lhsIdx;
        cjmpNode->cjmp_inst.operand2_index = rhsIdx;
    }

    if (token.token_type == AND) 
    {
        cjmpNode->cjmp_inst.target = noOpNode;
        append(head, tracker, cjmpNode);
        token = lexer.GetToken();
        parse_condition(head, tracker, noOpNode);
    } 
    else if (token.token_type == OR) 
    {
        struct InstructionNode* skipSecond = new InstructionNode();
        skipSecond->line_no = token.line_no;
        skipSecond->type = NOOP;
        cjmpNode->cjmp_inst.target = skipSecond;
        append(head, tracker, cjmpNode);
        struct InstructionNode* jumpToBody = new InstructionNode();
        jumpToBody->line_no = token.line_no;
        jumpToBody->type = JMP;
        append(head, tracker, jumpToBody);
        append(head, tracker, skipSecond);
        tracker = skipSecond;
        token = lexer.GetToken();
        parse_condition(head, tracker, noOpNode);
        struct InstructionNode* bodyEntry = new InstructionNode();
        bodyEntry->line_no = token.line_no;
        bodyEntry->type = NOOP;
        jumpToBody->jmp_inst.target = bodyEntry;
        append(head, tracker, bodyEntry);
        tracker = bodyEntry;
    } 
    else 
    {
        cjmpNode->cjmp_inst.target = noOpNode;
        append(head, tracker, cjmpNode);
    }
}

void parse_if_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* noOpNode = new InstructionNode(); noOpNode->type = NOOP;
    noOpNode->line_no = token.line_no;
    struct InstructionNode* condFailNode = new InstructionNode(); condFailNode->type = NOOP;
    condFailNode->line_no = token.line_no;
    token = lexer.GetToken();
    bool had_paren = maybe_consume_lparen(); // check for '('
    if (had_paren) token = lexer.GetToken();  // move into condition
    parse_condition(head, tracker, condFailNode);
    if (had_paren) token = lexer.GetToken();  // consume ')'
    token = lexer.GetToken();
    while (token.token_type != RBRACE) 
    { 
        parse_statement(head, tracker); token = lexer.GetToken(); 
    }
    struct InstructionNode* skipJmp = new InstructionNode();
    skipJmp->line_no = token.line_no;
    skipJmp->type = JMP; skipJmp->jmp_inst.target = noOpNode;
    append(head, tracker, skipJmp);
    append(head, tracker, condFailNode);
    tracker = condFailNode;

    Token peeked = lexer.peek(1);
    if (peeked.token_type == ELIF) 
    {
        token = lexer.GetToken();
        struct InstructionNode* elifHead = nullptr, *elifTracker = nullptr;
        parse_if_statement(elifHead, elifTracker);
        condFailNode->next = elifHead;
        if (elifTracker != nullptr) elifTracker->next = noOpNode;
        tracker = noOpNode;
    }
    else if (peeked.token_type == ELSE) 
    {
        token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
        while (token.token_type != RBRACE) 
        { 
            parse_statement(head, tracker); token = lexer.GetToken(); 
        }
        tracker->next = noOpNode; tracker = noOpNode;
    } 
    else 
    {
        condFailNode->next = noOpNode; tracker = noOpNode;
    }
}

void parse_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* noOpNode = new InstructionNode(); noOpNode->type = NOOP;
    noOpNode->line_no = token.line_no;
    struct InstructionNode* condStart = new InstructionNode(); condStart->type = NOOP;
    condStart->line_no = token.line_no;
    append(head, tracker, condStart);
    token = lexer.GetToken();
    bool had_paren = maybe_consume_lparen();
    if (had_paren) token = lexer.GetToken();
    parse_condition(head, tracker, noOpNode);
    if (had_paren) token = lexer.GetToken();
    token = lexer.GetToken();
    while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }
    struct InstructionNode* jmpNode = new InstructionNode();
    jmpNode->line_no = token.line_no;
    jmpNode->type = JMP; jmpNode->jmp_inst.target = condStart;
    tracker->next = jmpNode; jmpNode->next = noOpNode; tracker = noOpNode;
}

void parse_do_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // already parsed the keyword 'do'
    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->line_no = token.line_no;
    noOpNode->type = NOOP;
    struct InstructionNode* bodyStart = new InstructionNode();
    bodyStart->line_no = token.line_no;
    bodyStart->type = NOOP;

    // Mark top of loop body
    append(head, tracker, bodyStart);

    token = lexer.GetToken();  // consume '{'
    token = lexer.GetToken();  // first token inside body

    while (token.token_type != RBRACE)
    {
        parse_statement(head, tracker);
        token = lexer.GetToken();
    }
    // token = '}'

    token = lexer.GetToken();  // must be WHILE
    token = lexer.GetToken();
    bool had_paren = maybe_consume_lparen();
    if (had_paren) token = lexer.GetToken();

    // Condition: if TRUE -> jump back to bodyStart, if FALSE -> fall to noOpNode
    parse_condition(head, tracker, noOpNode);
    
    if (had_paren) token = lexer.GetToken();  // consume ')'

    // Unconditional jump back to body top (condition passed)
    struct InstructionNode* jmpBack = new InstructionNode();
    jmpBack->line_no = token.line_no;
    jmpBack->type = JMP;
    jmpBack->jmp_inst.target = bodyStart;
    append(head, tracker, jmpBack);
    append(head, tracker, noOpNode);
    tracker = noOpNode;
    // token is now ';' - outer loop advances past it
}

void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken(); token = lexer.GetToken();
    if (token.token_type == ID) parse_assignment_statement(head, tracker);

    struct InstructionNode* forConditionNode = new InstructionNode(); 
    forConditionNode->type = CJMP;
    forConditionNode->line_no = token.line_no;
    token = lexer.GetToken();
    if (token.token_type == ID && symbolTable.count(token.lexeme)) forConditionNode->cjmp_inst.operand1_index = symbolTable[token.lexeme];
    else { int s = alloc_slot(); mem[s] = stoi(token.lexeme); forConditionNode->cjmp_inst.operand1_index = s; }
    token = lexer.GetToken();
    if      (token.token_type == GREATER)  forConditionNode->cjmp_inst.condition_op = CONDITION_GREATER;
    else if (token.token_type == LESS)     forConditionNode->cjmp_inst.condition_op = CONDITION_LESS;
    else if (token.token_type == NOTEQUAL) forConditionNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
    token = lexer.GetToken();
    if (token.token_type == ID && symbolTable.count(token.lexeme)) forConditionNode->cjmp_inst.operand2_index = symbolTable[token.lexeme];
    else if (token.token_type == NUM) { int s = alloc_slot(); mem[s] = stoi(token.lexeme); forConditionNode->cjmp_inst.operand2_index = s; }

    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->line_no = token.line_no; 
    noOpNode->type = NOOP;
    forConditionNode->cjmp_inst.target = noOpNode;
    append(head, tracker, forConditionNode);

    token = lexer.GetToken(); token = lexer.GetToken();
    struct InstructionNode* updateNode = nullptr, *updateTracker = nullptr;
    if (token.token_type == ID && symbolTable.count(token.lexeme)) {
        parse_assignment_for_statement(updateNode, updateTracker);
        token = lexer.GetToken(); token = lexer.GetToken();
        while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }
        struct InstructionNode* jmpNode = new InstructionNode();
        jmpNode->line_no = token.line_no;
        jmpNode->type = JMP; jmpNode->jmp_inst.target = forConditionNode;
        tracker->next = updateNode; updateTracker->next = jmpNode; jmpNode->next = noOpNode; tracker = noOpNode;
    }
}

void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken();
    bool hasParen = (token.token_type == LPAREN);
    if (hasParen) token = lexer.GetToken();
    int switchSlot = alloc_temp();
    int resultIdx  = parse_expression(head, tracker);
    struct InstructionNode* copyNode = new InstructionNode();
    copyNode->line_no = token.line_no;
    copyNode->type = ASSIGN;
    copyNode->assign_inst.left_hand_side_index = switchSlot;
    copyNode->assign_inst.operand1_index       = resultIdx;
    copyNode->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, copyNode);
    if (hasParen && token.token_type == RPAREN) token = lexer.GetToken();
    token = lexer.GetToken();
    struct InstructionNode* noOpNode = new InstructionNode(); 
    noOpNode->line_no = token.line_no;
    noOpNode->type = NOOP;
    struct InstructionNode* lastCaseNode = nullptr;
    while (token.token_type == CASE || token.lexeme == "default") {
        if (token.token_type == CASE) 
        {
            token = lexer.GetToken();
            if (token.token_type == NUM) 
            {
                struct InstructionNode* caseNode = new InstructionNode();
                caseNode->line_no = token.line_no;
                caseNode->type = CJMP;
                caseNode->cjmp_inst.operand1_index = switchSlot;
                int s = alloc_slot();
                mem[s] = stoi(token.lexeme);
                caseNode->cjmp_inst.operand2_index = s;
                caseNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
                if (lastCaseNode) lastCaseNode->next = caseNode; lastCaseNode = caseNode;
                if (head == nullptr) { head = tracker = caseNode; } else { tracker->next = caseNode; tracker = caseNode; }
                token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
                struct InstructionNode* bodyHead = nullptr, *bodyTracker = nullptr;
                while (token.token_type != RBRACE) { parse_statement(bodyHead, bodyTracker); token = lexer.GetToken(); }
                caseNode->cjmp_inst.target = bodyHead;
                struct InstructionNode* jmpNode = new InstructionNode();
                jmpNode->line_no = token.line_no;
                jmpNode->type = JMP; jmpNode->jmp_inst.target = noOpNode;
                bodyTracker->next = jmpNode;
            }
        } 
        else if (token.lexeme == "default") 
        {
            struct InstructionNode* defaultNode = new InstructionNode();
            defaultNode->line_no = token.line_no; 
            defaultNode->type = JMP;
            if (lastCaseNode) lastCaseNode->next = defaultNode; lastCaseNode = defaultNode;
            if (head == nullptr) { head = tracker = defaultNode; } else { tracker->next = defaultNode; tracker = defaultNode; }
            token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
            struct InstructionNode* bodyHead = nullptr, *bodyTracker = nullptr;
            while (token.token_type != RBRACE) { parse_statement(bodyHead, bodyTracker); token = lexer.GetToken(); }
            defaultNode->jmp_inst.target = bodyHead;
            struct InstructionNode* jmpNode = new InstructionNode();
            jmpNode->line_no = token.line_no;
            jmpNode->type = JMP; 
            jmpNode->jmp_inst.target = noOpNode;
            bodyTracker->next = jmpNode;
        }
        token = lexer.GetToken();
    }
    if (lastCaseNode) lastCaseNode->next = noOpNode;
    tracker->next = noOpNode; tracker = noOpNode;
}

void parse_function_definition()
{
    token = lexer.GetToken();
    string funcName = token.lexeme;
    int    funcLine = token.line_no;
    if (functionTable.count(funcName)) report_error(funcLine, "duplicate function definition '" + funcName + "'");

    int slotBase = next_available;
    vector<string>  params;
    vector<VarType> paramTypes;

    // save symbol tables — restored after body so function-scoped names don't leak
    map<string, int>     savedSymbolTable       = symbolTable;
    map<string, VarType> savedTypeTable         = typeTable;
    map<string, int>     savedFloatSymbolTable  = floatSymbolTable;
    map<string, int>     savedDoubleSymbolTable = doubleSymbolTable;

    token = lexer.GetToken(); token = lexer.GetToken();
    while (token.token_type != RPAREN) {
        VarType paramType = TYPE_UNKNOWN;
        if      (token.token_type == INT_TYPE)    { paramType = TYPE_INT;    token = lexer.GetToken(); }
        else if (token.token_type == BOOL_TYPE)   { paramType = TYPE_BOOL;   token = lexer.GetToken(); }
        else if (token.token_type == STRING_TYPE) { paramType = TYPE_STRING; token = lexer.GetToken(); }
        else if (token.token_type == FLOAT_TYPE)  { paramType = TYPE_FLOAT;  token = lexer.GetToken(); }
        else if (token.token_type == DOUBLE_TYPE) { paramType = TYPE_DOUBLE; token = lexer.GetToken(); }
        if (token.token_type == ID) 
        {
            params.push_back(token.lexeme);
            paramTypes.push_back(paramType);
            int pSlot = (paramType == TYPE_FLOAT) ? alloc_float_slot() :
                        (paramType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            if (paramType == TYPE_FLOAT) floatSymbolTable[token.lexeme]  = pSlot;
            else if (paramType == TYPE_DOUBLE) doubleSymbolTable[token.lexeme] = pSlot;
            else symbolTable[token.lexeme] = pSlot;
            typeTable[token.lexeme]   = paramType;
        }
        token = lexer.GetToken();
        if (token.token_type == COMMA) token = lexer.GetToken();
    }

    functionParams[funcName]     = params;
    functionParamTypes[funcName] = paramTypes;

    // persist param slots before symbol table is restored at end of body
    {
        vector<int> pslots;
        for (int i = 0;i < (int)params.size();i++)
        {
            if (paramTypes[i] == TYPE_FLOAT) pslots.push_back(floatSymbolTable[params[i]]);
            else if (paramTypes[i] == TYPE_DOUBLE) pslots.push_back(doubleSymbolTable[params[i]]);
            else pslots.push_back(symbolTable[params[i]]); 
        }
        functionParamSlots[funcName] = pslots;
    }

    token = lexer.GetToken();  // ARROW or '{'
    vector<VarType> retTypes;
    vector<int> retSlots;
    if (token.token_type == ARROW) 
    {
        token = lexer.GetToken();  // first return type
        while (true)
        {
            VarType rt = TYPE_UNKNOWN;
            if      (token.token_type == INT_TYPE)    rt = TYPE_INT;
            else if (token.token_type == BOOL_TYPE)   rt = TYPE_BOOL;
            else if (token.token_type == STRING_TYPE) rt = TYPE_STRING;
            else if (token.token_type == FLOAT_TYPE) rt = TYPE_FLOAT;
            else if (token.token_type == DOUBLE_TYPE) rt = TYPE_DOUBLE;
            else report_error(token.line_no, "expected type after '->' or ','");
            retTypes.push_back(rt);
            int rs = (rt == TYPE_FLOAT) ? alloc_float_slot() : 
                     (rt == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            retSlots.push_back(rs);
            token = lexer.GetToken();  // ',' or '{'
            if (token.token_type != COMMA) break;
            token = lexer.GetToken();  // next return type
        }
        // token = '{'
    }
    else
    {
        retTypes.push_back(TYPE_UNKNOWN);
        retSlots.push_back(alloc_slot());
    }
    functionReturnTypesList[funcName] = retTypes;
    functionReturnSlots[funcName] = retSlots;
    functionReturnType[funcName] = retTypes[0];  // backward compatible
    functionReturnIndex[funcName] = retSlots[0];  // backward compatible
    int retIdx = retSlots[0];
    VarType retType = retTypes[0];
    token = lexer.GetToken();

    struct InstructionNode* head = nullptr, *tracker = nullptr;
    insideFunction     = true;
    currentFuncRetIdx  = retSlots[0];
    currentFuncRetType = retTypes[0];
    currentFuncRetSlots = retSlots;
    currentFuncRetTypes = retTypes;
    functionTable[funcName] = nullptr;

    while (token.token_type != RBRACE) 
    {
        if (token.token_type == RETURN) 
        {
            token = lexer.GetToken();
            vector<int> retExprs;
            retExprs.push_back(parse_expression(head, tracker));
            while (token.token_type == COMMA)
            {
                token = lexer.GetToken();
                retExprs.push_back(parse_expression(head, tracker));
            }
            if (retExprs.size() != retSlots.size())
            {
                report_error(token.line_no, "function '" + funcName + "' expects " + to_string(retSlots.size()) + " return value(s) but got " + to_string(retExprs.size()));
            }
            for (int i = 0;i < (int)retExprs.size() && i < (int)retSlots.size();i++)
            {
                VarType exprType = (i == 0) ? lastExprType : TYPE_UNKNOWN;
                bool tensorToInt = (exprType == TYPE_TENSOR && retTypes[i] == TYPE_INT);
                if (retTypes[i] != TYPE_UNKNOWN && exprType != TYPE_UNKNOWN && exprType != retTypes[i] && !tensorToInt)
                {
                    report_error(token.line_no, "return value " + to_string(i+1) + " type mismatch in '" + funcName + "'");
                }
                struct InstructionNode* an = new InstructionNode();
                an->line_no = token.line_no;
                if (retTypes[i] == TYPE_FLOAT)
                {
                    an->type = ASSIGN_F;
                    an->assign_f_inst.left_hand_side_index = retSlots[i];
                    an->assign_f_inst.operand1_index = retExprs[i];
                    an->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (retTypes[i] == TYPE_DOUBLE)
                {
                    an->type = ASSIGN_D;
                    an->assign_d_inst.left_hand_side_index = retSlots[i];
                    an->assign_d_inst.operand1_index = retExprs[i];
                    an->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    an->type = ASSIGN;
                    an->assign_inst.left_hand_side_index = retSlots[i];
                    an->assign_inst.operand1_index = retExprs[i];
                    an->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, an);
            }
            struct InstructionNode* retNode = new InstructionNode();
            retNode->line_no = token.line_no;
            retNode->type = RET;
            retNode->ret_inst.ret_val_index = retSlots[0];
            append(head, tracker, retNode);
        } 
        else 
        {
            parse_statement(head, tracker);
        }
        token = lexer.GetToken();
    }

    insideFunction = false; 
    currentFuncRetIdx = -1; 
    currentFuncRetType = TYPE_UNKNOWN;
    currentFuncRetSlots.clear();
    currentFuncRetTypes.clear();

    // restore symbol tables — remove function-scoped params and locals
    symbolTable       = savedSymbolTable;
    typeTable         = savedTypeTable;
    floatSymbolTable  = savedFloatSymbolTable;
    doubleSymbolTable = savedDoubleSymbolTable;

    struct InstructionNode* fallbackRet = new InstructionNode();
    fallbackRet->line_no = token.line_no;
    fallbackRet->type = RET; 
    fallbackRet->ret_inst.ret_val_index = retIdx;
    append(head, tracker, fallbackRet);
    functionTable[funcName] = head;

    int slotCount = next_available - slotBase;
    functionSlotRange[funcName] = {slotBase, slotCount};

    struct InstructionNode* fixNode = head;
    while (fixNode != nullptr) 
    {
        if (fixNode->type == CALL && fixNode->call_inst.function_head == nullptr) 
        {
            fixNode->call_inst.function_head   = head;
            fixNode->call_inst.func_slot_base  = slotBase;
            fixNode->call_inst.func_slot_count = slotCount;
        }
        fixNode = fixNode->next;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_repl_input  —  called by run_repl() in compiler.cc for each REPL entry
//
//  Reinitializes the lexer from the accumulated string, then parses either:
//    - one or more function definitions (def ...)
//    - a braced block  { statements }
//    - bare statements (no braces) until EOF
//
//  Symbol table, function table, and mem[] persist across calls so variables
//  and functions defined earlier in the REPL session remain available.
// ─────────────────────────────────────────────────────────────────────────────
struct InstructionNode* parse_repl_input(const std::string& s)
{
    // clear only per-parse error state — do NOT clear symbolTable / functionTable
    errorList.clear();

    lexer.ReinitializeFromString(s + "\n");

    token = lexer.GetToken();

    // function definitions
    while (token.token_type == STRUCT || token.token_type == DEF || token.token_type == CLASS)
    {
        if (token.token_type == STRUCT) parse_struct_definition();
        else if (token.token_type == CLASS) parse_class_definition();
        else parse_function_definition();
        token = lexer.GetToken();
    }

    struct InstructionNode* head    = nullptr;
    struct InstructionNode* tracker = nullptr;

    if (token.token_type == LBRACE)
    {
        // braced block: { statements }
        token = lexer.GetToken();
        while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }
    }
    else if (token.token_type != END_OF_FILE)
    {
        // bare statement(s) without braces
        while (token.token_type != END_OF_FILE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }
    }

    if (!errorList.empty())
    {
        for (const string& err : errorList)
            cerr << err << "\n";
        // return a no-op so the REPL loop can continue
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->line_no = token.line_no;
        noOpNode->type = NOOP;
        noOpNode->next = nullptr;
        return noOpNode;
    }

    if (head == nullptr)
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->line_no = token.line_no;
        noOpNode->type = NOOP;
        noOpNode->next = nullptr;
        head = noOpNode;
    }

    return head;
}

struct InstructionNode* parse_generate_intermediate_representation()
{
    // if (!check_balanced_parens()) 
    // {
    //     for (const string& err : errorList) fprintf(stderr, "%s\n", err.c_str());
    //     exit(1);
    // }
    // fprintf(stderr, "PARSE START\n");
    // fflush(stderr);
    token = lexer.GetToken();
    // fprintf(stderr, "FIRST TOKEN: %d '%s'\n", token.token_type, token.lexeme.c_str());
    // fflush(stderr);
    while (token.token_type == STRUCT || token.token_type == DEF || token.token_type == CLASS)
    {
        if (token.token_type == STRUCT) parse_struct_definition();
        else if (token.token_type == CLASS) parse_class_definition();
        else parse_function_definition();
        token = lexer.GetToken();
    }

    struct InstructionNode* head = nullptr, *tracker = nullptr;
    if (token.token_type != MAIN)
    {
        report_error(token.line_no, "expected 'main' function but got '" + token.lexeme + "'");
    }
    else
    {
        token = lexer.GetToken();  // consume '('
        // fprintf(stderr, "TOKEN after main: %d '%s'\n", token.token_type, token.lexeme.c_str());
        // fflush(stderr);
        token = lexer.GetToken();  // consume ')'
        token = lexer.GetToken();  // consume '{'
        token = lexer.GetToken();  // advance to first statement in body
        // fprintf(stderr, "FIRST STATEMENT TOKEN: %d '%s'\n", token.token_type, token.lexeme.c_str());
        // fflush(stderr);
        // parsing the main body
        while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
        {
            parse_statement(head, tracker); 
            token = lexer.GetToken();
        }
    }
    
    if (head == nullptr) 
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->line_no = token.line_no;
        noOpNode->type = NOOP; head = tracker = noOpNode;
    }
    if (!errorList.empty()) 
    {
        for (const string& err : errorList) cerr << err << "\n";
        exit(1);
    }
    return head;
}