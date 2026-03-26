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
#include "compiler.h"
#include "lexer.h"

using namespace std;

vector<string> errorList;
bool insideFunction = false;
int  currentFuncRetIdx = -1;

map<string, int> symbolTable;
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

VarType lastExprType = TYPE_UNKNOWN;
VarType currentFuncRetType = TYPE_UNKNOWN;

string typeToString(VarType t)
{
    switch (t) {
        case TYPE_INT: return "int";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        default: return "unknown";
    }
}

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
void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_function_definition();
int parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* noOpNode);

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

void parse_typed_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    VarType declType = TYPE_UNKNOWN;
    if      (token.token_type == INT_TYPE)    declType = TYPE_INT;
    else if (token.token_type == BOOL_TYPE)   declType = TYPE_BOOL;
    else if (token.token_type == STRING_TYPE) declType = TYPE_STRING;

    token = lexer.GetToken();
    string name = token.lexeme;
    int nameLine = token.line_no;

    if (symbolTable.count(name) || arrayTable.count(name)) 
    {
        report_error(nameLine, "redeclaration of '" + name + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
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
    if (rhsType != TYPE_UNKNOWN && rhsType != declType)
    {
        report_error(nameLine, "cannot assign " + typeToString(rhsType) + " to " + typeToString(declType) + " variable '" + name + "'");
    }
    struct InstructionNode* node = new InstructionNode();
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[name];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
}

void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == INT_TYPE || token.token_type == BOOL_TYPE || token.token_type == STRING_TYPE) {
        parse_typed_declaration(head, tracker);
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
            int resultIdx = parse_expression(head, tracker);
            VarType retType = lastExprType;
            if (currentFuncRetType != TYPE_UNKNOWN && retType != TYPE_UNKNOWN && retType != currentFuncRetType)
                report_error(token.line_no, "cannot return " + typeToString(retType) + " from function expecting " + typeToString(currentFuncRetType));
            struct InstructionNode* assignNode = new InstructionNode();
            assignNode->type = ASSIGN;
            assignNode->assign_inst.left_hand_side_index = currentFuncRetIdx;
            assignNode->assign_inst.operand1_index       = resultIdx;
            assignNode->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, assignNode);
            struct InstructionNode* retNode = new InstructionNode();
            retNode->type = RET;
            retNode->ret_inst.ret_val_index = currentFuncRetIdx;
            append(head, tracker, retNode);
        }
    } 
    else if (token.token_type == ID) 
    {
        string name = token.lexeme;
        if (arrayTable.find(name) != arrayTable.end()) parse_assignment_statement(head, tracker);
        else if (functionTable.find(name) != functionTable.end() && lexer.peek(1).token_type == LPAREN) parse_function_call(head, tracker);
        else parse_assignment_statement(head, tracker);
    } 
    else if (token.token_type != RBRACE) 
    {
        report_error(token.line_no, "unexpected token '" + token.lexeme + "'");
    }
}

void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* newNode = new InstructionNode();
    newNode->type = OUT;
    newNode->output_inst.newline       = true;
    newNode->output_inst.is_string     = false;
    newNode->output_inst.is_string_var = false;

    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // content

    if (token.token_type == STRING) {
        newNode->output_inst.var_index = next_str_available++;
        newNode->output_inst.is_string = true;
        newNode->output_inst.is_string_var = false;
        strMem.push_back(token.lexeme);
        token = lexer.GetToken();
    } else {
        int resultIdx = parse_expression(head, tracker);
        VarType exprType = lastExprType;
        newNode->output_inst.var_index = resultIdx;
        if (exprType == TYPE_STRING) {
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
    callNode->type = CALL;
    callNode->call_inst.function_head   = functionTable[funcName];
    callNode->call_inst.ret_val_index   = functionReturnIndex[funcName];
    callNode->call_inst.func_slot_base  = functionSlotRange.count(funcName) ? functionSlotRange[funcName].first : 0;
    callNode->call_inst.func_slot_count = functionSlotRange.count(funcName) ? functionSlotRange[funcName].second : 0;
    callNode->call_inst.num_params      = (int)params.size();

    if (params.size() > 0) {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        for (int i = 0; i < (int)params.size(); i++) {
            pSlots[i] = symbolTable[params[i]];
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        callNode->call_inst.param_slots   = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    } else {
        callNode->call_inst.param_slots   = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
    }
    append(head, tracker, callNode);

    int freshSlot = alloc_temp();
    struct InstructionNode* copyRetNode = new InstructionNode();
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
        inNode->type = IN;
        inNode->input_inst.var_index = slot;
        append(head, tracker, inNode);
        lastExprType = TYPE_INT;
        return slot;
    } 
    else if (token.token_type == ID) 
    {
        if (functionTable.find(token.lexeme) != functionTable.end()) return parse_function_call(head, tracker);
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
        if (leftType != TYPE_UNKNOWN && leftType != TYPE_INT) report_error(opLine, "arithmetic requires int operands, got " + typeToString(leftType));
        token = lexer.GetToken();
        int right = parse_factor(head, tracker);
        VarType rightType = lastExprType;
        if (rightType != TYPE_UNKNOWN && rightType != TYPE_INT) report_error(opLine, "arithmetic requires int operands, got " + typeToString(rightType));
        int tmp = alloc_temp();
        struct InstructionNode* node = new InstructionNode();
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
            copySize->type = ASSIGN;
            copySize->assign_inst.left_hand_side_index = storedSizeSlot;
            copySize->assign_inst.operand1_index       = sizeSlot;
            copySize->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, copySize);

            // emit ALLOC — fills baseSlot at runtime
            struct InstructionNode* allocNode = new InstructionNode();
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
    if (lhsType != TYPE_UNKNOWN && rhsType != TYPE_UNKNOWN && lhsType != rhsType)
        report_error(lhs_line, "cannot assign " + typeToString(rhsType) + " to " + typeToString(lhsType) + " variable '" + lhs + "'");

    struct InstructionNode* node = new InstructionNode();
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
        if      (token.token_type == GREATER)  condOp = CONDITION_GREATER;
        else if (token.token_type == LESS)     condOp = CONDITION_LESS;
        else if (token.token_type == NOTEQUAL) condOp = CONDITION_NOTEQUAL;
        token = lexer.GetToken();

        int rhsIdx = parse_expression(head, tracker);
        VarType rhsCondType = lastExprType;

        bool isStringCmp = (lhsCondType == TYPE_STRING || rhsCondType == TYPE_STRING);

        struct InstructionNode* cjmpNode = new InstructionNode();
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
        bodyEntry->type = NOOP;
        cjmpNode->cjmp_inst.target = bodyEntry;
        append(head, tracker, cjmpNode);
        struct InstructionNode* skipNode = new InstructionNode();
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
        skipSecond->type = NOOP;
        cjmpNode->cjmp_inst.target = skipSecond;
        append(head, tracker, cjmpNode);
        struct InstructionNode* jumpToBody = new InstructionNode();
        jumpToBody->type = JMP;
        append(head, tracker, jumpToBody);
        append(head, tracker, skipSecond);
        tracker = skipSecond;
        token = lexer.GetToken();
        parse_condition(head, tracker, noOpNode);
        struct InstructionNode* bodyEntry = new InstructionNode();
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
    struct InstructionNode* condFailNode = new InstructionNode(); condFailNode->type = NOOP;
    token = lexer.GetToken();
    parse_condition(head, tracker, condFailNode);
    token = lexer.GetToken();
    while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }
    struct InstructionNode* skipJmp = new InstructionNode();
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
        while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }
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
    struct InstructionNode* condStart = new InstructionNode(); condStart->type = NOOP;
    append(head, tracker, condStart);
    token = lexer.GetToken();
    parse_condition(head, tracker, noOpNode);
    token = lexer.GetToken();
    while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }
    struct InstructionNode* jmpNode = new InstructionNode();
    jmpNode->type = JMP; jmpNode->jmp_inst.target = condStart;
    tracker->next = jmpNode; jmpNode->next = noOpNode; tracker = noOpNode;
}

void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken(); token = lexer.GetToken();
    if (token.token_type == ID) parse_assignment_statement(head, tracker);

    struct InstructionNode* forConditionNode = new InstructionNode(); forConditionNode->type = CJMP;
    token = lexer.GetToken();
    if (token.token_type == ID && symbolTable.count(token.lexeme)) forConditionNode->cjmp_inst.operand1_index = symbolTable[token.lexeme];
    else { int s = alloc_slot(); mem[s] = stoi(token.lexeme); forConditionNode->cjmp_inst.operand1_index = s; }
    token = lexer.GetToken();
    if      (token.token_type == GREATER)  forConditionNode->cjmp_inst.condition_op = CONDITION_GREATER;
    else if (token.token_type == LESS)     forConditionNode->cjmp_inst.condition_op = CONDITION_LESS;
    else if (token.token_type == NOTEQUAL) forConditionNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
    token = lexer.GetToken();
    if (token.token_type == ID && symbolTable.count(token.lexeme)) forConditionNode->cjmp_inst.operand2_index = symbolTable[token.lexeme];
    else if (token.token_type == NUM) { int s = alloc_slot(); mem[s] = stoi(token.lexeme); forConditionNode->cjmp_inst.operand2_index = next_available++; }

    struct InstructionNode* noOpNode = new InstructionNode(); noOpNode->type = NOOP;
    forConditionNode->cjmp_inst.target = noOpNode;
    append(head, tracker, forConditionNode);

    token = lexer.GetToken(); token = lexer.GetToken();
    struct InstructionNode* updateNode = nullptr, *updateTracker = nullptr;
    if (token.token_type == ID && symbolTable.count(token.lexeme)) {
        parse_assignment_for_statement(updateNode, updateTracker);
        token = lexer.GetToken(); token = lexer.GetToken();
        while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }
        struct InstructionNode* jmpNode = new InstructionNode();
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
    copyNode->type = ASSIGN;
    copyNode->assign_inst.left_hand_side_index = switchSlot;
    copyNode->assign_inst.operand1_index       = resultIdx;
    copyNode->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, copyNode);
    if (hasParen && token.token_type == RPAREN) token = lexer.GetToken();
    token = lexer.GetToken();
    struct InstructionNode* noOpNode = new InstructionNode(); noOpNode->type = NOOP;
    struct InstructionNode* lastCaseNode = nullptr;
    token = lexer.GetToken();
    while (token.token_type == CASE || token.lexeme == "default") {
        if (token.token_type == CASE) 
        {
            token = lexer.GetToken();
            if (token.token_type == NUM) 
            {
                struct InstructionNode* caseNode = new InstructionNode();
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
                jmpNode->type = JMP; jmpNode->jmp_inst.target = noOpNode;
                bodyTracker->next = jmpNode;
            }
        } 
        else if (token.lexeme == "default") 
        {
            struct InstructionNode* defaultNode = new InstructionNode(); defaultNode->type = JMP;
            if (lastCaseNode) lastCaseNode->next = defaultNode; lastCaseNode = defaultNode;
            if (head == nullptr) { head = tracker = defaultNode; } else { tracker->next = defaultNode; tracker = defaultNode; }
            token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
            struct InstructionNode* bodyHead = nullptr, *bodyTracker = nullptr;
            while (token.token_type != RBRACE) { parse_statement(bodyHead, bodyTracker); token = lexer.GetToken(); }
            defaultNode->jmp_inst.target = bodyHead;
            struct InstructionNode* jmpNode = new InstructionNode();
            jmpNode->type = JMP; jmpNode->jmp_inst.target = noOpNode;
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

    token = lexer.GetToken(); token = lexer.GetToken();
    while (token.token_type != RPAREN) {
        VarType paramType = TYPE_UNKNOWN;
        if      (token.token_type == INT_TYPE)    { paramType = TYPE_INT;    token = lexer.GetToken(); }
        else if (token.token_type == BOOL_TYPE)   { paramType = TYPE_BOOL;   token = lexer.GetToken(); }
        else if (token.token_type == STRING_TYPE) { paramType = TYPE_STRING; token = lexer.GetToken(); }
        if (token.token_type == ID) 
        {
            params.push_back(token.lexeme);
            paramTypes.push_back(paramType);
            symbolTable[token.lexeme] = alloc_slot();
            typeTable[token.lexeme]   = paramType;
        }
        token = lexer.GetToken();
        if (token.token_type == COMMA) token = lexer.GetToken();
    }

    functionParams[funcName]     = params;
    functionParamTypes[funcName] = paramTypes;

    int retIdx = alloc_slot();
    functionReturnIndex[funcName] = retIdx;

    token = lexer.GetToken();  // ARROW or '{'
    VarType retType = TYPE_UNKNOWN;
    if (token.token_type == ARROW) 
    {
        token = lexer.GetToken();
        if      (token.token_type == INT_TYPE)    retType = TYPE_INT;
        else if (token.token_type == BOOL_TYPE)   retType = TYPE_BOOL;
        else if (token.token_type == STRING_TYPE) retType = TYPE_STRING;
        else report_error(token.line_no, "expected type after '->'");
        token = lexer.GetToken();  // '{'
    }

    functionReturnType[funcName] = retType;
    token = lexer.GetToken();

    struct InstructionNode* head = nullptr, *tracker = nullptr;
    insideFunction     = true;
    currentFuncRetIdx  = retIdx;
    currentFuncRetType = retType;
    functionTable[funcName] = nullptr;

    while (token.token_type != RBRACE) 
    {
        if (token.token_type == RETURN) 
        {
            token = lexer.GetToken();
            int resultIdx = parse_expression(head, tracker);
            VarType retExprType = lastExprType;
            if (retType != TYPE_UNKNOWN && retExprType != TYPE_UNKNOWN && retExprType != retType)
                report_error(token.line_no, "cannot return " + typeToString(retExprType) + " from function '" + funcName + "' expecting " + typeToString(retType));
            struct InstructionNode* assignNode = new InstructionNode();
            assignNode->type = ASSIGN;
            assignNode->assign_inst.left_hand_side_index = retIdx;
            assignNode->assign_inst.operand1_index       = resultIdx;
            assignNode->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, assignNode);
            struct InstructionNode* retNode = new InstructionNode();
            retNode->type = RET; retNode->ret_inst.ret_val_index = retIdx;
            append(head, tracker, retNode);
        } 
        else 
        {
            parse_statement(head, tracker);
        }
        token = lexer.GetToken();
    }

    insideFunction = false; currentFuncRetIdx = -1; currentFuncRetType = TYPE_UNKNOWN;

    struct InstructionNode* fallbackRet = new InstructionNode();
    fallbackRet->type = RET; fallbackRet->ret_inst.ret_val_index = retIdx;
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
    while (token.token_type == DEF)
    {
        parse_function_definition();
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
        noOpNode->type = NOOP;
        noOpNode->next = nullptr;
        return noOpNode;
    }

    if (head == nullptr)
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->type = NOOP;
        noOpNode->next = nullptr;
        head = noOpNode;
    }

    return head;
}

struct InstructionNode* parse_generate_intermediate_representation()
{
    lexer.Initialize();
    if (!check_balanced_parens()) 
    {
        for (const string& err : errorList) fprintf(stderr, "%s\n", err.c_str());
        exit(1);
    }
    token = lexer.GetToken();
    while (token.token_type == DEF) 
    { 
        parse_function_definition(); token = lexer.GetToken(); 
    }

    struct InstructionNode* head = nullptr, *tracker = nullptr;
    token = lexer.GetToken();
    while (token.token_type != RBRACE) { parse_statement(head, tracker); token = lexer.GetToken(); }

    if (head == nullptr) 
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->type = NOOP; head = tracker = noOpNode;
    }
    if (!errorList.empty()) 
    {
        for (const string& err : errorList) cerr << err << "\n";
        exit(1);
    }
    return head;
}