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

// ── Error handling ────────────────────────────────────────────────────────────
vector<string> errorList;
bool insideFunction  = false;
int  currentFuncRetIdx = -1;

// ── Symbol tables ─────────────────────────────────────────────────────────────
map<string, int>           symbolTable;
LexicalAnalyzer            lexer;
Token                      token;

map<string, InstructionNode*>   functionTable;
map<string, vector<string>>     functionParams;
map<string, int>                functionReturnIndex;
map<string, int>                arrayTable;
map<string, int>                arraySizeTable;
map<string, pair<int,int>>      functionSlotRange;

// ── Forward declarations ──────────────────────────────────────────────────────
int  parse_expression(struct InstructionNode*& head, struct InstructionNode*& tracker);
int  parse_term(struct InstructionNode*& head, struct InstructionNode*& tracker);
int  parse_factor(struct InstructionNode*& head, struct InstructionNode*& tracker);

void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_assignment_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_assignment_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_if_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_function_definition();
int  parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker);
void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker,
                     struct InstructionNode* noOpNode);

// ── Error helpers ─────────────────────────────────────────────────────────────

void report_error(int line, string message)
{
    errorList.push_back("Error at line " + to_string(line) + ": " + message);
}

bool check_declared(string name, int line)
{
    if (symbolTable.find(name) == symbolTable.end())
    {
        report_error(line, "undeclared variable '" + name + "'");
        return false;
    }
    return true;
}

bool check_bounds(string arrName, int index, int line)
{
    if (arraySizeTable.find(arrName) == arraySizeTable.end())
    {
        report_error(line, "undeclared array '" + arrName + "'");
        return false;
    }
    int size = arraySizeTable[arrName];
    if (index < 0 || index >= size)
    {
        report_error(line, "array '" + arrName + "' index " +
            to_string(index) + " out of bounds (size " + to_string(size) + ")");
        return false;
    }
    return true;
}

// ── Balanced bracket pre-pass ─────────────────────────────────────────────────
bool check_balanced_parens()
{
    struct BracketFrame { char bracket; int line; };
    vector<BracketFrame> stk;
    bool valid = true;
    int i = 1;
    Token t = lexer.peek(i);

    while (t.token_type != END_OF_FILE)
    {
        if (t.token_type == LPAREN || t.token_type == LBRAC || t.token_type == LBRACE)
        {
            char c = (t.token_type == LPAREN) ? '(' :
                     (t.token_type == LBRAC)  ? '[' : '{';
            stk.push_back({c, t.line_no});
        }
        else if (t.token_type == RPAREN || t.token_type == RBRAC || t.token_type == RBRACE)
        {
            char expected_open = (t.token_type == RPAREN) ? '(' :
                                 (t.token_type == RBRAC)  ? '[' : '{';
            char close_char    = (t.token_type == RPAREN) ? ')' :
                                 (t.token_type == RBRAC)  ? ']' : '}';
            if (stk.empty())
            {
                report_error(t.line_no, string("unmatched closing '") + close_char + "'");
                valid = false;
                break;
            }
            BracketFrame top = stk.back();
            stk.pop_back();
            if (top.bracket != expected_open)
            {
                report_error(t.line_no,
                    string("mismatched bracket: opened '") + top.bracket +
                    "' on line " + to_string(top.line) +
                    " but closed with '" + close_char + "'");
                valid = false;
                break;
            }
        }
        i++;
        t = lexer.peek(i);
    }

    if (valid && !stk.empty())
    {
        for (auto& frame : stk)
            report_error(frame.line,
                string("unclosed '") + frame.bracket + "' - missing closing bracket");
        valid = false;
    }
    return valid;
}

// ── IR helpers ────────────────────────────────────────────────────────────────

static void append(struct InstructionNode*& head, struct InstructionNode*& tracker,
                   struct InstructionNode* node)
{
    if (head == nullptr) head = tracker = node;
    else { tracker->next = node; tracker = node; }
}

static int alloc_temp()
{
    mem[next_available] = 0;
    return next_available++;
}

// ── Auto-declare a variable on first assignment ───────────────────────────────
// Returns the allocated memory slot.
static int auto_declare(const string& name)
{
    if (symbolTable.find(name) == symbolTable.end())
    {
        symbolTable[name] = next_available;
        mem[next_available] = 0;
        next_available++;
    }
    return symbolTable[name];
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_statement  —  central dispatcher
//
//  ON ENTRY: token = first token of the statement
//  ON EXIT:  token = ';' of the statement  (outer loop calls GetToken after)
// ─────────────────────────────────────────────────────────────────────────────
void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == PRINT)
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
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
                token = lexer.GetToken();
        }
        else
        {
            token = lexer.GetToken();  // first token of return expression
            int resultIdx = parse_expression(head, tracker);
            // token is now ';'

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

        // array write: arr[i] = value ;
        if (arrayTable.find(name) != arrayTable.end())
        {
            parse_assignment_statement(head, tracker);
        }
        // standalone function call used for side effects: foo() ;
        else if (functionTable.find(name) != functionTable.end()
                 && lexer.peek(1).token_type == LPAREN)
        {
            parse_function_call(head, tracker);
            // token is now ';'
        }
        else
        {
            // variable assignment (auto-declares if new) or array() declaration
            parse_assignment_statement(head, tracker);
        }
    }
    else if (token.token_type != RBRACE)
    {
        report_error(token.line_no, "unexpected token '" + token.lexeme + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_output_statement  —  print(expr)  or  print("string")
//
//  Always adds a newline (Python behaviour).
//  ON EXIT: token = ';'
// ─────────────────────────────────────────────────────────────────────────────
void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // token = PRINT
    struct InstructionNode* newNode = new InstructionNode();
    newNode->type = OUT;
    newNode->output_inst.newline = true;   // always newline

    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first token inside parens

    if (token.token_type == STRING)
    {
        strMem[next_str_available] = token.lexeme;
        newNode->output_inst.var_index = next_str_available;
        newNode->output_inst.is_string = true;
        next_str_available++;
        token = lexer.GetToken();  // STRING → ')'
    }
    else
    {
        // expression: variable, arithmetic, function call, array read, etc.
        int resultIdx = parse_expression(head, tracker);
        newNode->output_inst.var_index = resultIdx;
        newNode->output_inst.is_string = false;
        // token is now ')'
    }

    // token is ')' in both branches
    token = lexer.GetToken();  // ')' → ';'
    // leave ';' for outer loop

    append(head, tracker, newNode);
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_function_call  —  funcName(arg1, arg2, ...)
//
//  ON ENTRY: token = function name ID
//  ON EXIT:  token = first token AFTER ')'
//  RETURNS:  memory index where return value will be stored
// ─────────────────────────────────────────────────────────────────────────────
int parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string funcName = token.lexeme;

    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first argument or ')'

    vector<int> argIndices;
    while (token.token_type != RPAREN)
    {
        int argIdx = parse_expression(head, tracker);
        argIndices.push_back(argIdx);
        if (token.token_type == COMMA)
            token = lexer.GetToken();  // next argument
    }

    // token is now ')'
    token = lexer.GetToken();  // advance past ')'

    vector<string>& params = functionParams[funcName];

    if (argIndices.size() != params.size())
        report_error(token.line_no,
            "function '" + funcName + "' expects " +
            to_string(params.size()) + " argument(s) but got " +
            to_string(argIndices.size()));

    struct InstructionNode* callNode = new InstructionNode();
    callNode->type = CALL;
    callNode->call_inst.function_head   = functionTable[funcName];
    callNode->call_inst.ret_val_index   = functionReturnIndex[funcName];
    callNode->call_inst.func_slot_base  = functionSlotRange.count(funcName)
                                          ? functionSlotRange[funcName].first : 0;
    callNode->call_inst.func_slot_count = functionSlotRange.count(funcName)
                                          ? functionSlotRange[funcName].second : 0;
    callNode->call_inst.num_params      = (int)params.size();

    if (params.size() > 0)
    {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        for (int i = 0; i < (int)params.size(); i++)
        {
            pSlots[i] = symbolTable[params[i]];
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        callNode->call_inst.param_slots    = pSlots;
        callNode->call_inst.arg_val_slots  = aSlots;
    }
    else
    {
        callNode->call_inst.param_slots   = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
    }

    append(head, tracker, callNode);

    // copy return value to a fresh slot so it survives subsequent calls
    int freshSlot = alloc_temp();
    struct InstructionNode* copyRetNode = new InstructionNode();
    copyRetNode->type = ASSIGN;
    copyRetNode->assign_inst.left_hand_side_index = freshSlot;
    copyRetNode->assign_inst.operand1_index       = functionReturnIndex[funcName];
    copyRetNode->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, copyRetNode);

    return freshSlot;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_factor  —  NUM | ID | (expression) | input() | input("prompt")
//
//  CONTRACT: on entry  token = first token of this factor
//            on exit   token = first token AFTER this factor (lookahead)
// ─────────────────────────────────────────────────────────────────────────────
int parse_factor(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == LPAREN)
    {
        token = lexer.GetToken();  // consume '(', point at inner expr
        int result = parse_expression(head, tracker);
        // token is now ')'
        token = lexer.GetToken();  // consume ')', advance past it
        return result;
    }
    else if (token.token_type == NUM)
    {
        int idx = next_available;
        mem[next_available++] = stoi(token.lexeme);
        token = lexer.GetToken();
        return idx;
    }
    else if (token.token_type == INPUT)
    {
        // input()  or  input("Enter value: ")
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // prompt string or ')'

        if (token.token_type == STRING)
        {
            token = lexer.GetToken(); 
        }
        // token is now ')'
        token = lexer.GetToken();  // advance past ')'

        // allocate a fresh slot — executor fills it from stdin at runtime
        int slot = alloc_temp();

        struct InstructionNode* inNode = new InstructionNode();
        inNode->type = IN;
        inNode->input_inst.var_index = slot;
        append(head, tracker, inNode);

        return slot;
    }
    else if (token.token_type == ID)
    {
        // ── function call ─────────────────────────────────────────────────────
        if (functionTable.find(token.lexeme) != functionTable.end())
            return parse_function_call(head, tracker);

        // ── array read: arr[index] ────────────────────────────────────────────
        if (arrayTable.find(token.lexeme) != arrayTable.end())
        {
            string arrName = token.lexeme;
            int    base    = arrayTable[arrName];

            token = lexer.GetToken();  // '['
            token = lexer.GetToken();  // first token of index expression
            int indexLine = token.line_no;

            if (token.token_type == NUM)
                check_bounds(arrName, stoi(token.lexeme), indexLine);

            int indexSlot = parse_expression(head, tracker);
            // token is now ']'
            token = lexer.GetToken();  // advance past ']'

            int tempSlot = alloc_temp();
            struct InstructionNode* node = new InstructionNode();
            node->type = ARRAY_READ;
            node->array_inst.base_index   = base;
            node->array_inst.index_slot   = indexSlot;
            node->array_inst.target_index = tempSlot;
            node->array_inst.array_size   = arraySizeTable[arrName];
            append(head, tracker, node);
            return tempSlot;
        }

        // ── regular variable ──────────────────────────────────────────────────
        if (!check_declared(token.lexeme, token.line_no))
        {
            token = lexer.GetToken();
            return 0;
        }
        int idx = symbolTable[token.lexeme];
        token = lexer.GetToken();
        return idx;
    }

    // should never reach here on valid input
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_term  —  factor  ((* | /)  factor)*
// ─────────────────────────────────────────────────────────────────────────────
int parse_term(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_factor(head, tracker);

    while (token.token_type == MULT || token.token_type == DIV)
    {
        ArithmeticOperatorType op =
            (token.token_type == MULT) ? OPERATOR_MULT : OPERATOR_DIV;

        token = lexer.GetToken();
        int right = parse_factor(head, tracker);

        int tmp = alloc_temp();
        struct InstructionNode* node = new InstructionNode();
        node->type = ASSIGN;
        node->assign_inst.left_hand_side_index = tmp;
        node->assign_inst.operand1_index       = left;
        node->assign_inst.operand2_index       = right;
        node->assign_inst.op                   = op;
        append(head, tracker, node);

        left = tmp;
    }
    return left;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_expression  —  term  ((+ | -)  term)*
// ─────────────────────────────────────────────────────────────────────────────
int parse_expression(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_term(head, tracker);

    while (token.token_type == PLUS || token.token_type == MINUS)
    {
        ArithmeticOperatorType op =
            (token.token_type == PLUS) ? OPERATOR_PLUS : OPERATOR_MINUS;

        token = lexer.GetToken();
        int right = parse_term(head, tracker);

        int tmp = alloc_temp();
        struct InstructionNode* node = new InstructionNode();
        node->type = ASSIGN;
        node->assign_inst.left_hand_side_index = tmp;
        node->assign_inst.operand1_index       = left;
        node->assign_inst.operand2_index       = right;
        node->assign_inst.op                   = op;
        append(head, tracker, node);

        left = tmp;
    }
    return left;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_assignment_statement  —  ID = expression ;
//                             OR   ID = array(N) ;
//                             OR   arr[index] = expression ;
//
//  Variables are AUTO-DECLARED on first assignment — no VAR needed.
//  ON EXIT: token = ';'  (outer loop's GetToken() skips it)
// ─────────────────────────────────────────────────────────────────────────────
void parse_assignment_statement(struct InstructionNode*& head,
                                struct InstructionNode*& tracker)
{
    string lhs      = token.lexeme;
    int    lhs_line = token.line_no;

    // ── array write: arr[index] = value ; ────────────────────────────────────
    if (arrayTable.find(lhs) != arrayTable.end())
    {
        token = lexer.GetToken();  // '['
        token = lexer.GetToken();  // first token of index expression
        int indexLine = token.line_no;

        if (token.token_type == NUM)
            check_bounds(lhs, stoi(token.lexeme), indexLine);

        int indexSlot = parse_expression(head, tracker);
        // token is now ']'

        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // first token of value expression

        int valueSlot = parse_expression(head, tracker);
        // token is now ';'

        struct InstructionNode* node = new InstructionNode();
        node->type = ARRAY_WRITE;
        node->array_inst.base_index   = arrayTable[lhs];
        node->array_inst.index_slot   = indexSlot;
        node->array_inst.target_index = valueSlot;
        node->array_inst.array_size   = arraySizeTable[lhs];
        append(head, tracker, node);
        return;
    }

    // ── array declaration: name = array(N) ; ─────────────────────────────────
    // peek ahead: next=EQUAL, token-after-next=ARRAY
    if (symbolTable.find(lhs) == symbolTable.end()
        && lexer.peek(1).token_type == EQUAL
        && lexer.peek(2).token_type == ARRAY)
    {
        if (arrayTable.find(lhs) != arrayTable.end())
            report_error(lhs_line, "duplicate array declaration '" + lhs + "'");

        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // 'array' keyword
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // size (NUM)

        if (token.token_type != NUM)
        {
            report_error(lhs_line, "array size must be a number");
            return;
        }

        int size = stoi(token.lexeme);
        if (size <= 0)
        {
            report_error(lhs_line,
                "array '" + lhs + "' must have size greater than 0");
            return;
        }

        token = lexer.GetToken();  // ')'
        token = lexer.GetToken();  // ';'
        // leave ';' for outer loop

        arrayTable[lhs]     = next_available;
        arraySizeTable[lhs] = size;
        for (int i = 0; i < size; i++)
        {
            mem[next_available] = 0;
            next_available++;
        }
        return;
    }

    // ── regular variable assignment — auto-declare if first time ─────────────
    if (symbolTable.find(lhs) == symbolTable.end())
    {
        // first assignment to this name — auto-declare it
        symbolTable[lhs]    = next_available;
        mem[next_available] = 0;
        next_available++;
    }

    token = lexer.GetToken();  // consume ID, now token = '='
    token = lexer.GetToken();  // consume '=', now token = RHS start

    int resultIdx = parse_expression(head, tracker);
    // token is now ';'

    struct InstructionNode* node = new InstructionNode();
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[lhs];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
    // Leave token = ';' for the outer loop
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_assignment_for_statement  —  ID = expression
//
//  Used ONLY for the for-loop update clause (no semicolon).
//  ON EXIT: token = ')'  (closing paren of for header)
// ─────────────────────────────────────────────────────────────────────────────
void parse_assignment_for_statement(struct InstructionNode*& head,
                                    struct InstructionNode*& tracker)
{
    string lhs = token.lexeme;
    token = lexer.GetToken();  // '='
    token = lexer.GetToken();  // RHS start

    int resultIdx = parse_expression(head, tracker);
    // token is now ')' — for-header closing paren

    struct InstructionNode* node = new InstructionNode();
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[lhs];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_condition  —  handles single, and, or, not conditions
//
//  Emits CJMP nodes that jump to noOpNode if the overall condition fails.
//  ON ENTRY: token = first token of condition
//  ON EXIT:  token = '{' (opening brace of the block body)
// ─────────────────────────────────────────────────────────────────────────────
void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker,
                     struct InstructionNode* noOpNode)
{
    // ── not ───────────────────────────────────────────────────────────────────
    if (token.token_type == NOT)
    {
        token = lexer.GetToken();  // first token of negated condition

        struct InstructionNode* cjmpNode = new InstructionNode();
        cjmpNode->type = CJMP;

        int lhsIdx = parse_expression(head, tracker);
        cjmpNode->cjmp_inst.operand1_index = lhsIdx;

        if      (token.token_type == GREATER)  cjmpNode->cjmp_inst.condition_op = CONDITION_GREATER;
        else if (token.token_type == LESS)     cjmpNode->cjmp_inst.condition_op = CONDITION_LESS;
        else if (token.token_type == NOTEQUAL) cjmpNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;

        token = lexer.GetToken();  // first token of RHS
        int rhsIdx = parse_expression(head, tracker);
        cjmpNode->cjmp_inst.operand2_index = rhsIdx;

        // NOT logic:
        //   condition TRUE  → JMP noOpNode → skip body
        //   condition FALSE → bodyEntry    → enter body
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

        //token = lexer.GetToken();  // should be '{'
        return;
    }

    // ── parse first (or only) condition ───────────────────────────────────────
    struct InstructionNode* cjmpNode = new InstructionNode();
    cjmpNode->type = CJMP;

    int lhsIdx = parse_expression(head, tracker);
    cjmpNode->cjmp_inst.operand1_index = lhsIdx;

    if      (token.token_type == GREATER)  cjmpNode->cjmp_inst.condition_op = CONDITION_GREATER;
    else if (token.token_type == LESS)     cjmpNode->cjmp_inst.condition_op = CONDITION_LESS;
    else if (token.token_type == NOTEQUAL) cjmpNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;

    token = lexer.GetToken();  // first token of RHS
    int rhsIdx = parse_expression(head, tracker);
    cjmpNode->cjmp_inst.operand2_index = rhsIdx;
    // token is now 'and', 'or', or '{'

    // ── and ───────────────────────────────────────────────────────────────────
    if (token.token_type == AND)
    {
        cjmpNode->cjmp_inst.target = noOpNode;
        append(head, tracker, cjmpNode);
        token = lexer.GetToken();
        parse_condition(head, tracker, noOpNode);
    }
    // ── or ────────────────────────────────────────────────────────────────────
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
    // ── single condition ──────────────────────────────────────────────────────
    else
    {
        cjmpNode->cjmp_inst.target = noOpNode;
        append(head, tracker, cjmpNode);
        // token is now '{' — ready for body parsing
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_if_statement  —  if cond { body } [elif cond { body }]* [else { body }]
// ─────────────────────────────────────────────────────────────────────────────
void parse_if_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // absolute end of the entire if/elif/else chain
    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->type = NOOP;

    // where to land when this condition fails
    struct InstructionNode* condFailNode = new InstructionNode();
    condFailNode->type = NOOP;

    token = lexer.GetToken();  // first token of condition
    parse_condition(head, tracker, condFailNode);
    token = lexer.GetToken();  // first token of body

    while (token.token_type != RBRACE)
    {
        parse_statement(head, tracker);
        token = lexer.GetToken();
    }
    // token = '}' (if-body closing brace)

    struct InstructionNode* skipJmp = new InstructionNode();
    skipJmp->type = JMP;
    skipJmp->jmp_inst.target = noOpNode;
    append(head, tracker, skipJmp);

    append(head, tracker, condFailNode);
    tracker = condFailNode;

    Token peeked = lexer.peek(1);

    if (peeked.token_type == ELIF)
    {
        // consume '}' → token becomes ELIF
        token = lexer.GetToken();
        // token = ELIF — parse_if_statement advances past it to get condition

        struct InstructionNode* elifHead    = nullptr;
        struct InstructionNode* elifTracker = nullptr;

        parse_if_statement(elifHead, elifTracker);
        // returns with token = '}'

        condFailNode->next = elifHead;
        if (elifTracker != nullptr)
            elifTracker->next = noOpNode;
        tracker = noOpNode;
    }
    else if (peeked.token_type == ELSE)
    {
        token = lexer.GetToken();  // '}' → ELSE
        token = lexer.GetToken();  // ELSE → '{'
        token = lexer.GetToken();  // first token of else body

        while (token.token_type != RBRACE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }
        // token = '}' (else closing brace)

        tracker->next = noOpNode;
        tracker = noOpNode;
    }
    else
    {
        // no elif / else
        condFailNode->next = noOpNode;
        tracker = noOpNode;
        // token = '}' — outer loop's GetToken advances past it
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_while_statement  —  while cond { body }
// ─────────────────────────────────────────────────────────────────────────────
void parse_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->type = NOOP;

    struct InstructionNode* condStart = new InstructionNode();
    condStart->type = NOOP;
    append(head, tracker, condStart);

    token = lexer.GetToken();  // first token of condition
    parse_condition(head, tracker, noOpNode);
    // token is now '{'

    token = lexer.GetToken();  // first token of body

    while (token.token_type != RBRACE)
    {
        parse_statement(head, tracker);
        token = lexer.GetToken();
    }

    struct InstructionNode* jmpNode = new InstructionNode();
    jmpNode->type = JMP;
    jmpNode->jmp_inst.target = condStart;

    tracker->next  = jmpNode;
    jmpNode->next  = noOpNode;
    tracker        = noOpNode;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_for_statement  —  for(init ; cond ; update) { body }
// ─────────────────────────────────────────────────────────────────────────────
void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // init variable (ID)

    // init clause — auto-declares if needed
    if (token.token_type == ID)
        parse_assignment_statement(head, tracker);  // parses "i = 0 ;" leaves token = ';'

    // condition clause
    struct InstructionNode* forConditionNode = new InstructionNode();
    forConditionNode->type = CJMP;

    token = lexer.GetToken();  // LHS of condition (consumes the ';')
    if (token.token_type == ID && symbolTable.find(token.lexeme) != symbolTable.end())
        forConditionNode->cjmp_inst.operand1_index = symbolTable[token.lexeme];
    else
    {
        mem[next_available] = stoi(token.lexeme);
        forConditionNode->cjmp_inst.operand1_index = next_available++;
    }

    token = lexer.GetToken();  // condition operator
    if      (token.token_type == GREATER)  forConditionNode->cjmp_inst.condition_op = CONDITION_GREATER;
    else if (token.token_type == LESS)     forConditionNode->cjmp_inst.condition_op = CONDITION_LESS;
    else if (token.token_type == NOTEQUAL) forConditionNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;

    token = lexer.GetToken();  // RHS of condition
    if (token.token_type == ID && symbolTable.find(token.lexeme) != symbolTable.end())
        forConditionNode->cjmp_inst.operand2_index = symbolTable[token.lexeme];
    else if (token.token_type == NUM)
    {
        mem[next_available] = stoi(token.lexeme);
        forConditionNode->cjmp_inst.operand2_index = next_available++;
    }

    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->type = NOOP;
    forConditionNode->cjmp_inst.target = noOpNode;
    append(head, tracker, forConditionNode);

    // update clause
    token = lexer.GetToken();  // ';' after condition RHS
    token = lexer.GetToken();  // update variable (ID)

    struct InstructionNode* updateNode    = nullptr;
    struct InstructionNode* updateTracker = nullptr;

    if (token.token_type == ID && symbolTable.find(token.lexeme) != symbolTable.end())
    {
        parse_assignment_for_statement(updateNode, updateTracker);
        // token is now ')'

        token = lexer.GetToken();  // '{'
        token = lexer.GetToken();  // first token inside body

        while (token.token_type != RBRACE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }

        struct InstructionNode* jmpNode = new InstructionNode();
        jmpNode->type = JMP;
        jmpNode->jmp_inst.target = forConditionNode;

        tracker->next      = updateNode;
        updateTracker->next = jmpNode;
        jmpNode->next      = noOpNode;
        tracker            = noOpNode;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_switch_statement  —  switch(expr) { case N: { } ... default: { } }
// ─────────────────────────────────────────────────────────────────────────────
void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken();  // '(' or first token of expression

    bool hasParen = (token.token_type == LPAREN);
    if (hasParen)
        token = lexer.GetToken();  // first token of expression

    // evaluate switch expression into a dedicated slot
    int switchSlot = alloc_temp();
    int resultIdx  = parse_expression(head, tracker);

    struct InstructionNode* copyNode = new InstructionNode();
    copyNode->type = ASSIGN;
    copyNode->assign_inst.left_hand_side_index = switchSlot;
    copyNode->assign_inst.operand1_index       = resultIdx;
    copyNode->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, copyNode);

    if (hasParen && token.token_type == RPAREN)
        token = lexer.GetToken();  // ')'

    // token is now '{'
    token = lexer.GetToken();  // first case/default

    struct InstructionNode* noOpNode     = new InstructionNode();
    noOpNode->type = NOOP;
    struct InstructionNode* lastCaseNode = nullptr;

    while (token.token_type == CASE || token.lexeme == "default")
    {
        if (token.token_type == CASE)
        {
            token = lexer.GetToken();  // case value (NUM)
            if (token.token_type == NUM)
            {
                struct InstructionNode* caseNode = new InstructionNode();
                caseNode->type = CJMP;
                caseNode->cjmp_inst.operand1_index = switchSlot;
                caseNode->cjmp_inst.operand2_index = next_available;
                mem[next_available++]              = stoi(token.lexeme);
                caseNode->cjmp_inst.condition_op   = CONDITION_NOTEQUAL;

                if (lastCaseNode) lastCaseNode->next = caseNode;
                lastCaseNode = caseNode;

                if (head == nullptr) { head = tracker = caseNode; }
                else { tracker->next = caseNode; tracker = caseNode; }

                token = lexer.GetToken();  // ':'
                token = lexer.GetToken();  // '{'
                token = lexer.GetToken();  // first statement

                struct InstructionNode* bodyHead    = nullptr;
                struct InstructionNode* bodyTracker = nullptr;

                while (token.token_type != RBRACE)
                {
                    parse_statement(bodyHead, bodyTracker);
                    token = lexer.GetToken();
                }

                caseNode->cjmp_inst.target = bodyHead;

                struct InstructionNode* jmpNode = new InstructionNode();
                jmpNode->type = JMP;
                jmpNode->jmp_inst.target = noOpNode;
                bodyTracker->next = jmpNode;
            }
        }
        else if (token.lexeme == "default")
        {
            struct InstructionNode* defaultNode = new InstructionNode();
            defaultNode->type = JMP;

            if (lastCaseNode) lastCaseNode->next = defaultNode;
            lastCaseNode = defaultNode;

            if (head == nullptr) { head = tracker = defaultNode; }
            else { tracker->next = defaultNode; tracker = defaultNode; }

            struct InstructionNode* bodyHead    = nullptr;
            struct InstructionNode* bodyTracker = nullptr;

            token = lexer.GetToken();  // ':'
            token = lexer.GetToken();  // '{'
            token = lexer.GetToken();  // first statement

            while (token.token_type != RBRACE)
            {
                parse_statement(bodyHead, bodyTracker);
                token = lexer.GetToken();
            }

            defaultNode->jmp_inst.target = bodyHead;

            struct InstructionNode* jmpNode = new InstructionNode();
            jmpNode->type = JMP;
            jmpNode->jmp_inst.target = noOpNode;
            bodyTracker->next = jmpNode;
        }
        token = lexer.GetToken();
    }

    if (lastCaseNode) lastCaseNode->next = noOpNode;
    tracker->next = noOpNode;
    tracker       = noOpNode;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_function_definition  —  def name(params) { body }
//
//  ON ENTRY: token = DEF
//  ON EXIT:  function registered in all tables
// ─────────────────────────────────────────────────────────────────────────────
void parse_function_definition()
{
    token = lexer.GetToken();  // function name
    string funcName = token.lexeme;
    int    funcLine = token.line_no;

    if (functionTable.find(funcName) != functionTable.end())
        report_error(funcLine, "duplicate function definition '" + funcName + "'");

    int slotBase = next_available;
    vector<string> params;

    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first param or ')'

    while (token.token_type != RPAREN)
    {
        if (token.token_type == ID)
        {
            params.push_back(token.lexeme);
            symbolTable[token.lexeme] = next_available;
            mem[next_available]       = 0;
            next_available++;
        }
        token = lexer.GetToken();
        if (token.token_type == COMMA)
            token = lexer.GetToken();
    }
    // token = ')'

    functionParams[funcName] = params;

    int retIdx = next_available;
    mem[next_available] = 0;
    next_available++;
    functionReturnIndex[funcName] = retIdx;

    token = lexer.GetToken();  // '{'
    token = lexer.GetToken();  // first token of body

    struct InstructionNode* head    = nullptr;
    struct InstructionNode* tracker = nullptr;

    insideFunction    = true;
    currentFuncRetIdx = retIdx;

    // register early so recursive calls are recognized
    functionTable[funcName] = nullptr;

    while (token.token_type != RBRACE)
    {
        if (token.token_type == RETURN)
        {
            token = lexer.GetToken();  // first token of return expression
            int resultIdx = parse_expression(head, tracker);
            // token is now ';'

            struct InstructionNode* assignNode = new InstructionNode();
            assignNode->type = ASSIGN;
            assignNode->assign_inst.left_hand_side_index = retIdx;
            assignNode->assign_inst.operand1_index       = resultIdx;
            assignNode->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, assignNode);

            struct InstructionNode* retNode = new InstructionNode();
            retNode->type = RET;
            retNode->ret_inst.ret_val_index = retIdx;
            append(head, tracker, retNode);
        }
        else
        {
            parse_statement(head, tracker);
        }
        token = lexer.GetToken();
    }
    // token = '}'

    insideFunction    = false;
    currentFuncRetIdx = -1;

    // fallback RET in case function has no explicit return
    struct InstructionNode* fallbackRet = new InstructionNode();
    fallbackRet->type = RET;
    fallbackRet->ret_inst.ret_val_index = retIdx;
    append(head, tracker, fallbackRet);

    functionTable[funcName] = head;

    int slotCount = next_available - slotBase;
    functionSlotRange[funcName] = {slotBase, slotCount};

    // fix up recursive CALL nodes that got nullptr during parsing
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
//  parse_repl_input  —  parse and execute one REPL input
//
//  Takes a complete input string (one statement or one function definition),
//  tokenizes it, parses it, and returns the IR head for execution.
//  Returns nullptr if input is empty or only whitespace.
//  Does NOT call exit() on error — REPL handles errors gracefully.
// ─────────────────────────────────────────────────────────────────────────────
struct InstructionNode* parse_repl_input(const string& input)
{
    // reinitialize lexer with new input string
    lexer.ReinitializeFromString(input + "\n");

    token = lexer.GetToken();

    // empty input
    if (token.token_type == END_OF_FILE)
    {
        return nullptr;
    }

    struct InstructionNode* head = nullptr;
    struct InstructionNode* tracker = nullptr;

    // parse function definition
    if (token.token_type == DEF)
    {
        parse_function_definition();
        return nullptr;
    }

    // single statement
    if (token.token_type == RETURN)
    {
        report_error(token.line_no, "return statement outside of a function");
    }
    
    parse_statement(head, tracker);

    if (head == nullptr)
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->type = NOOP;
        head = tracker = noOpNode;
    }

    return head;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_generate_intermediate_representation
//
//  New program structure (no VAR, no inputs at bottom):
//
//    def foo(a, b) { ... }   ← zero or more function definitions
//
//    {                        ← main body
//        x = input() ;
//        print(x) ;
//    }
// ─────────────────────────────────────────────────────────────────────────────
struct InstructionNode* parse_generate_intermediate_representation()
{
    lexer.Initialize();  // safe: called after freopen

    if (!check_balanced_parens())
    {
        for (const string& err : errorList)
            fprintf(stderr, "%s\n", err.c_str());
        exit(1);
    }

    token = lexer.GetToken();

    // ── parse function definitions ─────────────────────────────────────────
    while (token.token_type == DEF)
    {
        parse_function_definition();
        token = lexer.GetToken();  // next DEF or '{'
    }

    // ── token is now '{' for main body ────────────────────────────────────
    struct InstructionNode* head    = nullptr;
    struct InstructionNode* tracker = nullptr;

    token = lexer.GetToken();  // first token of main body

    while (token.token_type != RBRACE)
    {
        parse_statement(head, tracker);
        token = lexer.GetToken();
    }

    // no inputs-at-bottom parsing — input() handles stdin at runtime

    if (head == nullptr)
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->type = NOOP;
        head = tracker = noOpNode;
    }

    if (!errorList.empty())
    {
        for (const string& err : errorList)
            cerr << err << "\n";
        exit(1);
    }

    return head;
}