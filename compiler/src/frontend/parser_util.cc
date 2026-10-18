/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Parser helpers: error reporting, declaration/bounds/bracket checks, IR list append. Split out of parser.cc.
#include "parser_internal.h"

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

void append(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* node)
{
    if (head == nullptr) head = tracker = node;
    else { tracker->next = node; tracker = node; }
}

int alloc_temp()
{
    return alloc_slot();
}

// The conditions of if / elif / while / do-while and the value of switch must be in parentheses:
// if (x < 5). Called with token on the '(' (after the keyword); leaves token on the first token inside.
void expect_condition_lparen(const string& keyword)
{
    if (token.token_type == LPAREN)
    {
        token = lexer.GetToken();  // consume '('
        return;
    }
    report_error(token.line_no, "'" + keyword + "' needs its condition in parentheses: " + keyword + " (...)");
}

// Called with token on the ')' that closes the condition; leaves token on the token after it.
void expect_condition_rparen(const string& keyword)
{
    if (token.token_type == RPAREN)
    {
        token = lexer.GetToken();  // consume ')'
        return;
    }
    report_error(token.line_no, "missing ')' after the condition of '" + keyword + "'");
}
