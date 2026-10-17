// Recursive-descent parser entry points: function definitions, REPL input, whole-program parse.
// The rest of the parser is in parser_*.cc (see parser_internal.h).
#include "parser_internal.h"

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
                    // retSlots[i] is the function's own static return slot, resued as-is by every
                    // call site's ret_val_index/all_ret_slots - tagging it here, once, is enough
                    // for every caller to see it as tensor-tainted too.
                    if (g_tensorSlots.count(retExprs[i])) g_tensorSlots.insert(retSlots[i]);
                }
                append(head, tracker, an);
            }
            struct InstructionNode* retNode = new InstructionNode();
            retNode->line_no = token.line_no;
            retNode->type = RET;
            retNode->ret_inst.ret_val_index = retSlots[0];
            retNode->ret_inst.ret_type = retTypes[0];
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
    fallbackRet->ret_inst.ret_type = retType;
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
